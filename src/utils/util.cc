//--------------------------------------------------------------------------
// Copyright (C) 2014-2017 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2002-2013 Sourcefire, Inc.
// Copyright (C) 2002 Martin Roesch <roesch@sourcefire.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "util.h"

#include <fcntl.h>
#include <grp.h>
#include <luajit.h>
#include <netdb.h>
#include <openssl/crypto.h>
#include <pcap.h>
#include <pcre.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <zlib.h>

#ifdef HAVE_FLATBUFFERS
#include <flatbuffers/flatbuffers.h>
#endif

#ifdef HAVE_HYPERSCAN
#include <hs_compile.h>
#endif

#ifdef HAVE_LZMA
#include <lzma.h>
#endif

extern "C" {
#include <daq.h>
}

#include <fstream>

#include "log/messages.h"
#include "main/build.h"
#include "main/snort_config.h"
#include "main/snort_debug.h"
#include "packet_io/sfdaq.h"
#include "protocols/packet.h"   // For NUM_IP_PROTOS

#include "util_cstring.h"

#ifdef PATH_MAX
#define PATH_MAX_UTIL PATH_MAX
#else
#define PATH_MAX_UTIL 1024
#endif

char** protocol_names = NULL;

/****************************************************************************
 * Store interesting data in memory that would not otherwise be visible
 * in a CORE(5) file
 ***************************************************************************/
#define SNORT_VERSION_STRING ("### Snort Version " VERSION " Build " BUILD "\n")
#define SNORT_VERSION_STRLEN sizeof(SNORT_VERSION_STRING)
char __snort_version_string[SNORT_VERSION_STRLEN];

void StoreSnortInfoStrings()
{
    strncpy(__snort_version_string, SNORT_VERSION_STRING,
        sizeof(__snort_version_string));
}

#undef SNORT_VERSION_STRING
#undef SNORT_VERSION_STRLEN

/****************************************************************************
 *
 * Function: DisplayBanner()
 *
 * Purpose:  Show valuable proggie info
 *
 * Arguments: None.
 *
 * Returns: 0 all the time
 *
 ****************************************************************************/
int DisplayBanner()
{
    const char* info = getenv("HOSTTYPE");

    if ( !info )
        info="from 2.9.8-383";  // last sync with head

    const char* ljv = LUAJIT_VERSION;
    while ( *ljv && !isdigit(*ljv) )
        ++ljv;

    LogMessage("\n");
    LogMessage("   ,,_     -*> Snort++ <*-\n");
    LogMessage("  o\"  )~   Version %s (Build %s) %s\n",
        VERSION, BUILD, info);
    LogMessage("   ''''    By Martin Roesch & The Snort Team\n");
    LogMessage("           http://snort.org/contact#team\n");
    LogMessage("           Copyright (C) 2014-2017 Cisco and/or its affiliates."
                           " All rights reserved.\n");
    LogMessage("           Copyright (C) 1998-2013 Sourcefire, Inc., et al.\n");
    LogMessage("           Using DAQ version %s\n", daq_version_string());
    LogMessage("           Using %s\n", pcap_lib_version());
    LogMessage("           Using LuaJIT version %s\n", ljv);
    LogMessage("           Using PCRE version %s\n", pcre_version());
    LogMessage("           Using ZLIB version %s\n", zlib_version);
#ifdef HAVE_LZMA
    LogMessage("           Using LZMA version %s\n", lzma_version_string());
#endif
    LogMessage("           Using %s\n", SSLeay_version(SSLEAY_VERSION));
#ifdef HAVE_HYPERSCAN
    LogMessage("           Using Hyperscan version %s\n", hs_version());
#endif
#ifdef HAVE_FLATBUFFERS
    LogMessage("           Using %s\n", flatbuffers::flatbuffer_version_string);
#endif
    LogMessage("\n");

    return 0;
}

/****************************************************************************
 *
 * Function: ts_print(const struct, char *)
 *
 * Purpose: Generate a time stamp and stuff it in a buffer.  This one has
 *          millisecond precision.  Oh yeah, I ripped this code off from
 *          TCPdump, props to those guys.
 *
 * Arguments: timeval => clock struct coming out of libpcap
 *            timebuf => buffer to stuff timestamp into
 *
 * Returns: void function
 *
 ****************************************************************************/
void ts_print(const struct timeval* tvp, char* timebuf)
{
    int s;
    int localzone;
    time_t Time;
    struct timeval tv;
    struct timezone tz;
    struct tm* lt;    /* place to stick the adjusted clock data */

    /* if null was passed, we use current time */
    if (!tvp)
    {
        /* manual page (for linux) says tz is never used, so.. */
        memset((char*)&tz, 0, sizeof(tz));
        gettimeofday(&tv, &tz);
        tvp = &tv;
    }

    localzone = snort_conf->thiszone;

    /*
    **  If we're doing UTC, then make sure that the timezone is correct.
    */
    if (SnortConfig::output_use_utc())
        localzone = 0;

    s = (tvp->tv_sec + localzone) % 86400;
    Time = (tvp->tv_sec + localzone) - s;

    struct tm ttm;
    lt = gmtime_r(&Time, &ttm);

    if (SnortConfig::output_include_year())
    {
        int year = (lt->tm_year >= 100) ? (lt->tm_year - 100) : lt->tm_year;

        (void)SnortSnprintf(timebuf, TIMEBUF_SIZE,
            "%02d/%02d/%02d-%02d:%02d:%02d.%06u",
            lt->tm_mon + 1, lt->tm_mday, year,
            s / 3600, (s % 3600) / 60, s % 60,
            (u_int)tvp->tv_usec);
    }
    else
    {
        (void)SnortSnprintf(timebuf, TIMEBUF_SIZE,
            "%02d/%02d-%02d:%02d:%02d.%06u", lt->tm_mon + 1,
            lt->tm_mday, s / 3600, (s % 3600) / 60, s % 60,
            (u_int)tvp->tv_usec);
    }
}

/****************************************************************************
 *
 * Function: gmt2local(time_t)
 *
 * Purpose: Figures out how to adjust the current clock reading based on the
 *          timezone you're in.  Ripped off from TCPdump.
 *
 * Arguments: time_t => offset from GMT
 *
 * Returns: offset seconds from GMT
 *
 ****************************************************************************/
int gmt2local(time_t t)
{
    if (t == 0)
        t = time(NULL);

    struct tm gmt;
    gmtime_r(&t, &gmt);

    struct tm loc;
    localtime_r(&t, &loc);

    int dt = (loc.tm_hour - gmt.tm_hour) * 60 * 60 +
        (loc.tm_min - gmt.tm_min) * 60;

    int dir = loc.tm_year - gmt.tm_year;

    if (dir == 0)
        dir = loc.tm_yday - gmt.tm_yday;

    dt += dir * 24 * 60 * 60;

    return(dt);
}

static FILE* pid_lockfile = NULL;
static FILE* pid_file = NULL;

void CreatePidFile(pid_t pid)
{
    snort_conf->pid_filename = snort_conf->log_dir;
    snort_conf->pid_filename += "/snort.pid";

    std::string pid_lockfilename;

    if ( !SnortConfig::no_lock_pid_file() )
    {
        pid_lockfilename = snort_conf->pid_filename;
        pid_lockfilename += ".lck";
        int lock_fd;

        /* First, lock the PID file */
        pid_lockfile = fopen(pid_lockfilename.c_str(), "w");

        if ( pid_lockfile )
        {
            struct flock lock;
            lock_fd = fileno(pid_lockfile);

            lock.l_type = F_WRLCK;
            lock.l_whence = SEEK_SET;
            lock.l_start = 0;
            lock.l_len = 0;

            if (fcntl(lock_fd, F_SETLK, &lock) == -1)
            {
                ClosePidFile();
                ParseError("Failed to Lock PID File \"%s\" for PID \"%d\"",
                    snort_conf->pid_filename.c_str(), (int)pid);
                return;
            }
        }
    }

    /* Okay, were able to lock PID file, now open and write PID */
    pid_file = fopen(snort_conf->pid_filename.c_str(), "w");
    if (pid_file)
    {
        LogMessage("Writing PID \"%d\" to file \"%s\"\n", (int)pid,
            snort_conf->pid_filename.c_str());
        fprintf(pid_file, "%d\n", (int)pid);
        fflush(pid_file);
    }
    else
    {
        const char* error = get_error(errno);
        ErrorMessage("Failed to create pid file %s, Error: %s",
            snort_conf->pid_filename.c_str(), error);
        snort_conf->pid_filename.clear();
    }
    if ( !pid_lockfilename.empty() )
        unlink(pid_lockfilename.c_str());
}

/****************************************************************************
 *
 * Function: ClosePidFile(char *)
 *
 * Purpose:  Releases lock on a PID file
 *
 * Arguments: None
 *
 * Returns: void function
 *
 ****************************************************************************/
void ClosePidFile()
{
    if (pid_file)
    {
        fclose(pid_file);
        pid_file = NULL;
    }
    if (pid_lockfile)
    {
        fclose(pid_lockfile);
        pid_lockfile = NULL;
    }
}

/****************************************************************************
 *
 * Function: SetUidGid()
 *
 * Purpose:  Sets safe UserID and GroupID if needed
 *
 * Arguments: none
 *
 * Returns: void function
 *
 ****************************************************************************/
bool SetUidGid(int user_id, int group_id)
{
    // Were any changes requested?
    if (group_id == -1 && user_id == -1)
        return true;

    // FIXIT-L Move this check to Snort::drop_privileges()
    if (!SFDAQ::unprivileged())
    {
        ParseError("Cannot drop privileges - %s DAQ does not support unprivileged operation.\n", SFDAQ::get_type());
        return false;
    }

    if (group_id != -1)
    {
        if (setgid(group_id) < 0)
        {
            ParseError("Cannot set GID: %d", group_id);
            return false;
        }
        LogMessage("Set GID to %d\n", group_id);
    }

    if (user_id != -1)
    {
        if (setuid(user_id) < 0)
        {
            ParseError("Cannot set UID: %d", user_id);
            return false;
        }
        LogMessage("Set UID to %d\n", user_id);
    }

    return true;
}

/****************************************************************************
 *
 * Function: InitGroups()
 *
 * Purpose:  Sets the groups of the process based on the UserID with the
 *           GroupID added
 *
 * Arguments: none
 *
 * Returns: void function
 *
 ****************************************************************************/
void InitGroups(int user_id, int group_id)
{
    if ((user_id != -1) && (getuid() == 0))
    {
        struct passwd* pw = getpwuid(user_id);  // main thread only

        if (pw != NULL)
        {
            /* getpwuid and initgroups may use the same static buffers */
            char* username = snort_strdup(pw->pw_name);

            if (initgroups(username, group_id) < 0)
                ParseError("Can not initgroups(%s,%d)", username, group_id);

            snort_free(username);
        }

        /** Just to be on the safe side... **/
        endgrent();
        endpwent();
    }
}

//-------------------------------------------------------------------------

// FIXIT-L this is a duplicate of PacketManager::get_proto_name()
void InitProtoNames()
{
    if ( !protocol_names )
        protocol_names = (char**)snort_calloc(NUM_IP_PROTOS, sizeof(char*));

    for ( int i = 0; i < NUM_IP_PROTOS; i++ )
    {
        struct protoent* pt = getprotobynumber(i);  // main thread only

        if (pt != NULL)
        {
            protocol_names[i] = snort_strdup(pt->p_name);

            for ( size_t j = 0; j < strlen(protocol_names[i]); j++ )
                protocol_names[i][j] = toupper(protocol_names[i][j]);
        }
        else
        {
            char protoname[10];
            SnortSnprintf(protoname, sizeof(protoname), "PROTO:%03d", i);
            protocol_names[i] = snort_strdup(protoname);
        }
    }
}

void CleanupProtoNames()
{
    if (protocol_names != NULL)
    {
        int i;

        for (i = 0; i < NUM_IP_PROTOS; i++)
        {
            if (protocol_names[i] != NULL)
                snort_free(protocol_names[i]);
        }

        snort_free(protocol_names);
        protocol_names = NULL;
    }
}

/****************************************************************************
 *
 * Function: read_infile(const char* key, const char* file)
 *
 * Purpose: Reads the BPF filters in from a file.  Ripped from tcpdump.
 *
 * Arguments: fname => the name of the file containing the BPF filters
 *
 * Returns: the processed BPF string
 *
 ****************************************************************************/
std::string read_infile(const char* key, const char* fname)
{
    int fd = open(fname, O_RDONLY);
    struct stat buf;

    if (fstat(fd, &buf) < 0)
    {
        ParseError("can't stat %s: %s", fname, get_error(errno));
        return "";
    }

    //check that its a regular file and not a directory or special file
    if (!S_ISREG(buf.st_mode) )
    {
        ParseError("not a regular file: %s", fname);
        return "";
    }

    std::string line;
    std::ifstream bpf_file(fname);

    if (bpf_file.is_open())
    {
        std::stringstream file_content;
        file_content << bpf_file.rdbuf();
        line = file_content.str();

        bpf_file.close();
    }
    else
    {
        ParseError("can't open file %s = %s: %s", key, fname, get_error(errno));
    	return "";	
    }

    return line;
}

char* snort_strndup(const char* src, size_t dst_size)
{
    char* dup = (char*)snort_calloc(dst_size + 1);

    if ( SnortStrncpy(dup, src, dst_size + 1) == SNORT_STRNCPY_ERROR )
    {
        snort_free(dup);
        return nullptr;
    }

    return dup;
}

char* snort_strdup(const char* str)
{
    assert(str);
    size_t n = strlen(str) + 1;
    char* p = (char*)snort_alloc(n);
    memcpy(p, str, n);
    return p;
}

typedef char PathBuf[PATH_MAX_UTIL+1];

static const char* CurrentWorkingDir(PathBuf& buf)
{
    if ( !getcwd(buf, sizeof(buf)-1) )
        return nullptr;

    buf[sizeof(buf)-1] = '\0';
    return buf;
}

static char* GetAbsolutePath(const char* dir, PathBuf& buf)
{
    assert(dir);
    errno = 0;

    if ( !realpath(dir, buf) )
    {
        LogMessage("Couldn't determine absolute path for '%s': %s\n", dir, get_error(errno));
        return nullptr;
    }

    return buf;
}

/**
 * Chroot and adjust the snort_conf->log_dir reference
 */
bool EnterChroot(std::string& root_dir, std::string& log_dir)
{
    if (log_dir.empty())
    {
        ParseError("Log directory not specified");
        return false;
    }
    PathBuf pwd;
    DebugFormat(DEBUG_INIT, "EnterChroot: %s\n", CurrentWorkingDir(pwd));
    PathBuf abs_log_dir;

    if ( !GetAbsolutePath(log_dir.c_str(), abs_log_dir) )
        return false;

    /* change to the desired root directory */
    if (chdir(root_dir.c_str()) != 0)
    {
        ParseError("EnterChroot: Can not chdir to \"%s\": %s", root_dir.c_str(),
            get_error(errno));
        return false;
    }

    /* always returns an absolute pathname */
    const char* abs_root_dir = CurrentWorkingDir(pwd);
    if (!abs_root_dir)
    {
        ParseError("Couldn't retrieve current working directory");
        return false;
    }
    size_t abs_root_dir_len = strlen(abs_root_dir);
    DebugFormat(DEBUG_INIT, "ABS: %s %zu\n", abs_root_dir, abs_root_dir_len);

    if (strncmp(abs_root_dir, abs_log_dir, abs_root_dir_len))
    {
        ParseError("Specified log directory is not contained with the chroot jail");
        return false;
    }

    if (chroot(abs_root_dir) < 0)
    {
        ParseError("Can not chroot to \"%s\": absolute: %s: %s",
            root_dir.c_str(), abs_root_dir, get_error(errno));
        return false;
    }

    DebugFormat(DEBUG_INIT,"chroot success (%s ->", abs_root_dir);
    DebugFormat(DEBUG_INIT,"%s)\n ", CurrentWorkingDir(pwd));

    /* Immediately change to the root directory of the jail. */
    if (chdir("/") < 0)
    {
        ParseError("Can not chdir to \"/\" after chroot: %s",
            get_error(errno));
        return false;
    }

    DebugFormat(DEBUG_INIT,"chdir success (%s)\n", CurrentWorkingDir(pwd));

    if (abs_root_dir_len >= strlen(abs_log_dir))
        log_dir = "/";
    else
        log_dir = abs_log_dir + abs_root_dir_len;

    DebugFormat(DEBUG_INIT,"new logdir from %s to %s\n", abs_log_dir, log_dir.c_str());

    LogMessage("Chroot directory = %s\n", root_dir.c_str());

    return true;
}

#if defined(NOCOREFILE)
void SetNoCores()
{
    struct rlimit rlim;

    getrlimit(RLIMIT_CORE, &rlim);
    rlim.rlim_max = 0;
    setrlimit(RLIMIT_CORE, &rlim);
}
#endif

const char* get_error(int errnum)
{
    static THREAD_LOCAL char buf[128];

#if (defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE < 200112L && \
        defined(_XOPEN_SOURCE) && _XOPEN_SOURCE < 600) || _GNU_SOURCE
    return strerror_r(errnum, buf, sizeof(buf));
#else
    (void)strerror_r(errnum, buf, sizeof(buf));
    return buf;
#endif
}

