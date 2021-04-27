//--------------------------------------------------------------------------
// Copyright (C) 2018-2021 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// http2_flow_data.cc author Tom Peters <thopeter@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "http2_flow_data.h"

#include "service_inspectors/http_inspect/http_inspect.h"
#include "service_inspectors/http_inspect/http_test_manager.h"

#include "http2_enum.h"
#include "http2_frame.h"
#include "http2_module.h"
#include "http2_push_promise_frame.h"
#include "http2_start_line.h"
#include "http2_stream.h"

using namespace snort;
using namespace Http2Enums;
using namespace HttpCommon;

unsigned Http2FlowData::inspector_id = 0;

#ifdef REG_TEST
uint64_t Http2FlowData::instance_count = 0;
#endif

// Each stream will have class Http2Stream allocated and a node in streams list
const size_t Http2FlowData::stream_memory_size = sizeof(std::_List_node<Http2Stream*>) + sizeof(class Http2Stream);
const size_t Http2FlowData::stream_increment_memory_size = stream_memory_size *
    STREAM_MEMORY_TRACKING_INCREMENT;

Http2FlowData::Http2FlowData(Flow* flow_) :
    FlowData(inspector_id),
    flow(flow_),
    hi((HttpInspect*)(flow->assistant_gadget)),
    hpack_decoder { Http2HpackDecoder(this, SRC_CLIENT, events[SRC_CLIENT],
                        infractions[SRC_CLIENT]),
                    Http2HpackDecoder(this, SRC_SERVER, events[SRC_SERVER],
                        infractions[SRC_SERVER]) },
    data_cutter { Http2DataCutter(this, SRC_CLIENT), Http2DataCutter(this, SRC_SERVER) }
{
    if (hi != nullptr)
    {
        hi_ss[SRC_CLIENT] = hi->get_splitter(true);
        hi_ss[SRC_SERVER] = hi->get_splitter(false);
    }

#ifdef REG_TEST
    seq_num = ++instance_count;
    if (HttpTestManager::use_test_output(HttpTestManager::IN_HTTP2) &&
        !HttpTestManager::use_test_input(HttpTestManager::IN_HTTP2))
    {
        printf("HTTP/2 Flow Data construct %" PRIu64 "\n", seq_num);
        fflush(nullptr);
    }
#endif
    Http2Module::increment_peg_counts(PEG_CONCURRENT_SESSIONS);
    if (Http2Module::get_peg_counts(PEG_MAX_CONCURRENT_SESSIONS) <
        Http2Module::get_peg_counts(PEG_CONCURRENT_SESSIONS))
        Http2Module::increment_peg_counts(PEG_MAX_CONCURRENT_SESSIONS);
}

Http2FlowData::~Http2FlowData()
{
#ifdef REG_TEST
    if (HttpTestManager::use_test_output(HttpTestManager::IN_HTTP2) &&
        !HttpTestManager::use_test_input(HttpTestManager::IN_HTTP2))
    {
        printf("HTTP/2 Flow Data destruct %" PRIu64 "\n", seq_num);
        fflush(nullptr);
    }
#endif
    if (Http2Module::get_peg_counts(PEG_CONCURRENT_SESSIONS) > 0)
        Http2Module::decrement_peg_counts(PEG_CONCURRENT_SESSIONS);

    for (int k=0; k <= 1; k++)
    {
        delete infractions[k];
        delete events[k];
        delete hi_ss[k];
        delete[] frame_data[k];
    }

    for (Http2Stream* stream : streams)
        delete stream;
    // Since stream memory is allocated in blocks of 25, must also deallocate in blocks of 25 to
    // ensure consistent rounding.
    while (stream_memory_allocations_tracked > STREAM_MEMORY_TRACKING_INCREMENT)
        update_stream_memory_deallocations();
}

HttpFlowData* Http2FlowData::get_hi_flow_data() const
{
    assert(stream_in_hi != Http2Enums::NO_STREAM_ID);
    Http2Stream* stream = get_hi_stream();
    return stream->get_hi_flow_data();
}

void Http2FlowData::set_hi_flow_data(HttpFlowData* flow)
{
    assert(stream_in_hi != Http2Enums::NO_STREAM_ID);
    Http2Stream* stream = get_hi_stream();
    stream->set_hi_flow_data(flow);
}

size_t Http2FlowData::size_of()
{
    // Account for memory for 25 concurrent streams up front, plus 1 stream for stream id 0.
    return sizeof(*this) + stream_increment_memory_size + stream_memory_size +
        (2 * sizeof(Http2EventGen)) + (2 * sizeof(Http2Infractions));
}

void Http2FlowData::update_stream_memory_allocations()
{
    assert(concurrent_streams > stream_memory_allocations_tracked);
    assert(concurrent_streams % stream_memory_allocations_tracked == 1);
    update_allocations(stream_increment_memory_size);
    stream_memory_allocations_tracked += STREAM_MEMORY_TRACKING_INCREMENT;
}

void Http2FlowData::update_stream_memory_deallocations()
{
    assert(stream_memory_allocations_tracked >= STREAM_MEMORY_TRACKING_INCREMENT);
    update_deallocations(stream_increment_memory_size);
    stream_memory_allocations_tracked -= STREAM_MEMORY_TRACKING_INCREMENT;
}

Http2Stream* Http2FlowData::find_stream(const uint32_t key) const
{
    for (Http2Stream* stream : streams)
    {
        if (stream->get_stream_id() == key)
            return stream;
    }

    return nullptr;
}

Http2Stream* Http2FlowData::get_processing_stream(const SourceId source_id, uint32_t concurrent_streams_limit)
{
    const uint32_t key = processing_stream_id;
    class Http2Stream* stream = find_stream(key);
    if (!stream)
    {
        if (concurrent_streams >= concurrent_streams_limit)
        {
            *infractions[source_id] += INF_TOO_MANY_STREAMS;
            events[source_id]->create_event(EVENT_TOO_MANY_STREAMS);
            Http2Module::increment_peg_counts(PEG_FLOWS_OVER_STREAM_LIMIT);
            abort_flow[SRC_CLIENT] = true;
            abort_flow[SRC_SERVER] = true;
            return nullptr;
        }

        // Verify stream id is bigger than all previous streams initiated by same side
        if (key != 0)
        {
            const bool non_housekeeping_frame = frame_type[source_id] == FT_HEADERS ||
                                                frame_type[source_id] == FT_DATA ||
                                                frame_type[source_id] == FT_PUSH_PROMISE;
            if (non_housekeeping_frame)
            {
                // If we see both sides of traffic, odd stream id should be initiated by client,
                // even by server. If we can't see one side, can't guarantee order
                const bool is_on_expected_side = (key % 2 != 0 && source_id == SRC_CLIENT) ||
                                                 (key % 2 == 0 && source_id == SRC_SERVER);
                if (is_on_expected_side)
                {
                    if (key <= max_stream_id[source_id])
                    {
                        *infractions[source_id] += INF_INVALID_STREAM_ID;
                        events[source_id]->create_event(EVENT_INVALID_STREAM_ID);
                        return nullptr;
                    }
                    else
                        max_stream_id[source_id] = key;
                }
            }
            else // housekeeping frame
            {
                // Delete stream after this frame is evaluated.
                // Prevents recreating and keeping already completed streams for
                // housekeeping frames
                delete_stream = true;
            }
        }

        // Allocate new stream
        stream = new Http2Stream(key, this);
        streams.emplace_front(stream);

        // stream 0 does not count against stream limit
        if (key > 0)
        {
            concurrent_streams += 1;
            if (concurrent_streams > Http2Module::get_peg_counts(PEG_MAX_CONCURRENT_STREAMS))
                Http2Module::increment_peg_counts(PEG_MAX_CONCURRENT_STREAMS);
            if (concurrent_streams > stream_memory_allocations_tracked)
                update_stream_memory_allocations();
        }
    }
    return stream;
}

void Http2FlowData::delete_processing_stream()
{
    std::list<Http2Stream*>::iterator it;
    for (it = streams.begin(); it != streams.end(); ++it)
    {
        if ((*it)->get_stream_id() == processing_stream_id)
        {
            delete *it;
            streams.erase(it);
            delete_stream = false;
            assert(concurrent_streams > 0);
            concurrent_streams -= 1;
            return;
        }
    }
    assert(false);
}

Http2Stream* Http2FlowData::get_hi_stream() const
{
    return find_stream(stream_in_hi);
}

Http2Stream* Http2FlowData::find_current_stream(const SourceId source_id) const
{
    return find_stream(current_stream[source_id]);
}

Http2Stream* Http2FlowData::find_processing_stream() const
{
    return find_stream(get_processing_stream_id());
}

uint32_t Http2FlowData::get_processing_stream_id() const
{
    return processing_stream_id;
}

// processing stream is the current stream except for push promise frames with properly formatted
// promised stream IDs
void Http2FlowData::set_processing_stream_id(const SourceId source_id)
{
    assert(processing_stream_id == NO_STREAM_ID);
    if (frame_type[source_id] == FT_PUSH_PROMISE)
        processing_stream_id = Http2PushPromiseFrame::get_promised_stream_id(
            events[source_id], infractions[source_id], frame_data[source_id],
            frame_data_size[source_id]);
    if (processing_stream_id == NO_STREAM_ID)
        processing_stream_id = current_stream[source_id];
}

uint32_t Http2FlowData::get_current_stream_id(const SourceId source_id) const
{
    return current_stream[source_id];
}

void Http2FlowData::allocate_hi_memory(HttpFlowData* hi_flow_data)
{
    update_allocations(hi_flow_data->size_of());
}

void Http2FlowData::deallocate_hi_memory(HttpFlowData* hi_flow_data)
{
    update_deallocations(hi_flow_data->size_of());
}

bool Http2FlowData::is_mid_frame() const
{
    return (header_octets_seen[SRC_SERVER] != 0) || (remaining_data_padding[SRC_SERVER] != 0) ||
        continuation_expected[SRC_SERVER];
}

