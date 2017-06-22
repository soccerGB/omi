/*
**==============================================================================
**
** Copyright (c) Microsoft Corporation. All rights reserved. See file LICENSE
** for license information.
**
**==============================================================================
*/

#include <stdlib.h>
#include <time.h>
#include <sock/sock.h>
#include <pal/dir.h>
#include <server/server.h>
#include <libgen.h>

#define S_SOCKET_LENGTH 8
#define S_SECRET_STRING_LENGTH 32
#define SOCKET_FILE_NAME_LENGTH 10
#define FILE_OWNERSHIP_MAX 24

typedef struct _FileOwnershipInfo
{
    const char *file;
    uid_t uid;
    gid_t gid;
} FileOwnershipInfo;

static FileOwnershipInfo s_fileOwnershipInfo[FILE_OWNERSHIP_MAX];
static unsigned int s_filesChanged = 0;
static Options s_opts;
static ServerData s_data;

static const ZChar HELP[] = ZT("\
Usage: %s [OPTIONS]\n\
\n\
This program starts the server.\n\
\n\
OPTIONS:\n\
    -h, --help                  Print this help message.\n\
    -d                          Daemonize the server process (POSIX only).\n\
    -s                          Stop the server process (POSIX only).\n\
    -r                          Re-read configuration by the running server (POSIX only).\n\
    --reload-dispatcher         Re-read configuration by the running server (POSIX only), but don't unload providers.\n\
    --httpport PORT             HTTP protocol listener port.\n\
    --httpsport PORT            HTTPS protocol listener port.\n\
    --idletimeout TIMEOUT       Idle providers unload timeout (in seconds).\n\
    -v, --version               Print version information.\n\
    -l, --logstderr             Send log output to standard error.\n\
    --loglevel LEVEL            Set logging level to one of the following\n\
                                symbols/numbers: fatal/0, error/1, warning/2,\n\
                                info/3, debug/4, verbose/5 (default 2).\n\
    --httptrace                 Enable logging of HTTP traffic.\n\
    --timestamp                 Print timestamp server was built with.\n\
    --nonroot                   Run in non-root mode.\n\
    --service ACCT              Use ACCT as the service account.\n\
\n");

static void _AddFileOwnershipInfo(const char *file, uid_t uid, gid_t gid)
{
    if (s_filesChanged >= FILE_OWNERSHIP_MAX - 1)
    {
        err(PAL_T("Too many files are changing ownership"));
    }

    s_fileOwnershipInfo[s_filesChanged].file = file;
    s_fileOwnershipInfo[s_filesChanged].uid = uid;
    s_fileOwnershipInfo[s_filesChanged].gid = gid;

    s_filesChanged++;
}

static void _ChangeOwnership(const char *path)
{
    struct stat buffer;
    int r;

    if (stat(path, &buffer) == 0)
    {
        if (buffer.st_uid != s_opts.serviceAccountUID  ||  buffer.st_gid != s_opts.serviceAccountGID)
        {
            r = chown(path, s_opts.serviceAccountUID, s_opts.serviceAccountGID);
            if (r != 0)
            {
                err(PAL_T("failed to chown path: %s"), scs(path));
            }
            _AddFileOwnershipInfo(PAL_Strdup(path), buffer.st_uid, buffer.st_gid);
            trace_PathOwnershipChanged(path);
        }
    }
}

static void _ChangePathOwnership(const char *path, MI_Boolean isDirectory)
{
    _ChangeOwnership(path);
    if (!isDirectory)
    {
        char stringBuf[PAL_MAX_PATH_SIZE];
        char *directoryName;

        strcpy(stringBuf, path);
        directoryName = dirname(stringBuf);
        _ChangeOwnership(directoryName);
    }
}

static void _RevertFileOwnership()
{
    int i;
    int r;

    if (0 != IsRoot() || s_opts.nonRoot == MI_FALSE || s_filesChanged == 0)
        return;

    for (i=0; i<s_filesChanged; ++i)
    {
        r = chown(s_fileOwnershipInfo[i].file, s_fileOwnershipInfo[i].uid, s_fileOwnershipInfo[i].gid);
        if (r != 0)
        {
            printf("Error: Unable to change ownership back: %s\n", 
                   s_fileOwnershipInfo[i].file);
        }
        PAL_Free((void*)s_fileOwnershipInfo[i].file);
    }
}

static MI_Result _GiveEnginePermissions()
{
    const char *clientSockFile = OMI_GetPath(ID_SOCKETFILE);
    const char *keyFile = OMI_GetPath(ID_KEYFILE);
    const char *authDir = OMI_GetPath(ID_AUTHDIR);
    const char *localStateDir = OMI_GetPath(ID_LOCALSTATEDIR);
    const char *homeDir = getenv("HOME");

    char *clientSockDir;
    char *ntlmUserFile = getenv("NTLM_USER_FILE");

    char stringBuf[PAL_MAX_PATH_SIZE];

    /* Verify that server is started as root */
    if (0 != IsRoot() )
    {
        // Can't do anything
        return MI_RESULT_OK;
    }

    if (s_opts.serviceAccountUID <= 0 ||  s_opts.serviceAccountGID <= 0)
    {
        err(PAL_T("No valid service account provided"));
    }

    /* Make sure engine can create socket file (for comm with client) */
    
    strcpy(stringBuf, clientSockFile);
    clientSockDir = dirname(stringBuf);
    _ChangePathOwnership(clientSockDir, MI_TRUE);

    /* Make sure engine can read ssl private key */
    _ChangePathOwnership(keyFile, MI_FALSE);

    /* Make sure engine can read auth files */
    _ChangePathOwnership(authDir, MI_TRUE);

    /* Make sure engine can write to var dir */
    _ChangePathOwnership(localStateDir, MI_TRUE);

    /* NTLM file */
    if (s_opts.ntlmCredFile)
    {
        _ChangePathOwnership(s_opts.ntlmCredFile, MI_FALSE);
    }

    if (ntlmUserFile)
    {
        _ChangePathOwnership(ntlmUserFile, MI_FALSE);
    }

    strcpy(stringBuf, homeDir);
    strcat(stringBuf, "/.omi/ntlmcred");
    _ChangePathOwnership(stringBuf, MI_FALSE);

    return MI_RESULT_OK;
}

static int _StartEngine(int argc, char** argv, const char *engineSockFile, const char *secretString)
{
    Sock s[2];
    char engineFile[PAL_MAX_PATH_SIZE];
    pid_t child;
    int fdLimit;
    int fd;
    int size;
    char socketString[S_SOCKET_LENGTH];
    MI_Result r;
    const char *binDir = OMI_GetPath(ID_BINDIR);

    Strlcpy(engineFile, binDir, PAL_MAX_PATH_SIZE);
    Strlcat(engineFile, "/omiengine", PAL_MAX_PATH_SIZE);
    argv[0] = engineFile;

    r = BinaryProtocolListenFile(engineSockFile, &s_data.mux[0], &s_data.protocol0, secretString);
    if (r != MI_RESULT_OK)
    {
        return -1;
    }
    
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, s) != 0)
    {
        err(ZT("failed to create unix-domain socket pair"));
    }

    if (MI_RESULT_OK != Sock_SetBlocking(s[0], MI_FALSE) ||
        MI_RESULT_OK != Sock_SetBlocking(s[1], MI_FALSE))
    {
        trace_SetNonBlocking_Failed();
        return -1;
    }

    child = fork();
    if (child < 0)
    {
        err(PAL_T("fork failed"));
    }

    if (child > 0)   // parent
    {
        s_data.enginePid = child;
        trace_ServerClosingSocket(0, s[1]);
        Sock_Close(s[1]);

        r = BinaryProtocolListenSock(s[0], &s_data.mux[1], &s_data.protocol1, engineSockFile, secretString);
        if (r != MI_RESULT_OK)
        {
            return -1;
        }

        return 0;
    }

    // child code here

    trace_EngineClosingSocket(0, s[0]);
    Sock_Close(s[0]);

    if (0 == IsRoot())
    {
        if (SetUser(s_opts.serviceAccountUID, s_opts.serviceAccountGID) != 0)
        {
            err(PAL_T("failed to change uid/gid of engine"));
        }  
    }

    /* Close all open file descriptors except provided socket
     (Some systems have UNLIMITED of 2^64; limit to something reasonable) */

    fdLimit = getdtablesize();
    if (fdLimit > 2500 || fdLimit < 0)
    {
        fdLimit = 2500;
    }

    /* ATTN: close first 3 also! Left for debugging only */
    for (fd = 3; fd < fdLimit; ++fd)
    {
        if (fd != s[1])
            close(fd);
    }

    argv[argc-1] = int64_to_a(socketString, S_SOCKET_LENGTH, (long long)s[1], &size);

    execv(argv[0], argv);
    err(PAL_T("Launch failed: %d"), errno);
    exit(1);
}

static char** _DuplicateArgv(int argc, const char* argv[])
{
    int i;

    char **newArgv = (char**)malloc((argc+3)*sizeof(char*));

    // argv[0] will be filled in later
    if (argc > 1)
    {
        for (i = 1; i<argc; ++i)
        {
            newArgv[i] = (char*)argv[i];
        }
    }

    newArgv[argc] = "--socketpair";
    newArgv[argc+1] = NULL;  // to be filled later
    newArgv[argc+2] = NULL;

    return newArgv;
}

static int _GenerateRandomString(char *buffer, int bufLen)
{
    const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    time_t t;
    int i;
    unsigned int availableLetters = sizeof(letters) - 1;

    srand((unsigned) time(&t));
    
    for (i = 0; i < bufLen - 1; ++i)
    {
        buffer[i] = letters[rand() % availableLetters];
    }

    buffer[bufLen - 1] = '\0';
    return 0;
}

static int _CreateSockFile(char *sockFileBuf, int sockFileBufSize, char *secretStringBuf, int secretStringBufSize)
{
    char sockDir[PAL_MAX_PATH_SIZE];
    char file[PAL_MAX_PATH_SIZE];
    char name[SOCKET_FILE_NAME_LENGTH];

    Strlcpy(sockDir, OMI_GetPath(ID_SYSCONFDIR), PAL_MAX_PATH_SIZE);
    Strlcat(sockDir, "/sockets", PAL_MAX_PATH_SIZE);

    Dir *dir = Dir_Open(sockDir);
    if (dir)
    {
        DirEnt *entry;
        while((entry = Dir_Read(dir)))
        {
            if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0)
                continue;

            Strlcpy(file, sockDir, PAL_MAX_PATH_SIZE);
            Strlcat(file, "/", PAL_MAX_PATH_SIZE);
            Strlcat(file, entry->name, PAL_MAX_PATH_SIZE);
            //printf("Removing %s...\n", file);
            unlink(file);
        }
    }
    else
    {
        int r;

        if (0 == IsRoot())
        {
            r = Mkdir(sockDir, 0700);
            if (r != 0)
            {
                err(PAL_T("failed to create sockets directory: %s"), scs(sockDir));
            }

            r = chown(sockDir, s_opts.serviceAccountUID, s_opts.serviceAccountGID);
            if (r != 0)
            {
                err(PAL_T("failed to chown sockets directory: %s"), scs(sockDir));
            }
        }
        else
        {
            r = Mkdir(sockDir, 0755);
            if (r != 0)
            {
                err(PAL_T("failed to create sockets directory: %s"), scs(sockDir));
            }
        }
    }
        
    if ( _GenerateRandomString(name, SOCKET_FILE_NAME_LENGTH) != 0)
    {
        err(PAL_T("Unable to generate socket file name"));
    }

    if ( _GenerateRandomString(secretStringBuf, secretStringBufSize) != 0)
    {
        err(PAL_T("Unable to generate secretString"));
    }

    Strlcpy(sockFileBuf, sockDir, sockFileBufSize);
    Strlcat(sockFileBuf, "/omi_", sockFileBufSize);
    Strlcat(sockFileBuf, name, sockFileBufSize);

    return 0;
}

static void _GetCommandLineNonRootOption(
    int* argc_,
    const char* argv[])
{
    int argc = *argc_;
    int i;
    s_opts.nonRoot = MI_FALSE;

    for (i = 1; i < argc; )
    {
        if (strncmp(argv[i], "--nonroot", 10) == 0)
        {
            s_opts.nonRoot = MI_TRUE;
            memmove((char*)&argv[i], (char*)&argv[i+1], 
                sizeof(char*) * (argc-i));

            argc -= 1;
        }
        else
            i++;
    }

    *argc_ = argc;
}

int servermain(int argc, const char* argv[])
{
#if defined(CONFIG_POSIX)
    int pidfile = -1;
#endif
    int engine_argc = 0;
    char **engine_argv = NULL;
    char socketFile[PAL_MAX_PATH_SIZE];
    char secretString[S_SECRET_STRING_LENGTH];
    const char* arg0 = argv[0];
    MI_Result result;    

    SetDefaults(&s_opts, &s_data, arg0, OMI_SERVER);

    // Determine if we're running with non-root option
    _GetCommandLineNonRootOption(&argc, argv);

    /* pass all command-line args, except --nonroot, to engine */
    engine_argc = argc + 2;
    engine_argv = _DuplicateArgv(argc, argv);

    /* Get --destdir command-line option */
    GetCommandLineDestDirOption(&argc, argv);

    /* Extract configuration file options */
    GetConfigFileOptions();

    /* Extract command-line options a second time (to override) */
    GetCommandLineOptions(&argc, argv);

    /* Open the log file */
    OpenLogFile();

    /* Print help */
    if (s_opts.help)
    {
        Ftprintf(stderr, HELP, scs(arg0));
        exit(1);
    }

    /* Print locations of files and directories */
    if (s_opts.locations)
    {
        PrintPaths();
        Tprintf(ZT("\n"));
        exit(0);
    }

#if defined(CONFIG_POSIX)
    if (s_opts.stop || s_opts.reloadConfig)
    {
        if (PIDFile_IsRunning() != 0)
        {
            info_exit(ZT("server is not running\n"));
        }

        if (PIDFile_Signal(s_opts.stop ? SIGTERM : SIGHUP) != 0)
        {
            err(ZT("failed to stop server\n"));
        }

        if (s_opts.stop)
        {
            Tprintf(ZT("%s: stopped server\n"), scs(arg0));
        }
        else
        {
            Tprintf(ZT("%s: refreshed server\n"), scs(arg0));
        }

        exit(0);
    }
    if (s_opts.reloadDispatcher)
    {
        if (PIDFile_IsRunning() != 0)
        {
            info_exit(ZT("server is not running\n"));
        }

        if (PIDFile_Signal(SIGUSR1) != 0)
        {
            err(ZT("failed to reload dispatcher on the server\n"));
        }

        Tprintf(ZT("%s: server has reloaded its dispatcher\n"), scs(arg0));

        exit(0);        
    }
#endif

#if defined(CONFIG_POSIX)

    if (PIDFile_IsRunning() == 0)
    {
        err(ZT("server is already running\n"));
    }

    /* Verify that server is started as root */
    if (0 != IsRoot() && !s_opts.ignoreAuthentication)
    {
        err(ZT("expected to run as root"));
    }

    /* ATTN: unit-test support; should be removed/ifdefed later */
    if (s_opts.ignoreAuthentication)
    {
        IgnoreAuthCalls(1);
    }

    /* Watch for SIGTERM signals */
    if (0 != SetSignalHandler(SIGTERM, HandleSIGTERM) ||
        0 != SetSignalHandler(SIGHUP, HandleSIGHUP) ||
        0 != SetSignalHandler(SIGUSR1, HandleSIGUSR1))
    {
        err(ZT("cannot set sighandler, errno %d"), errno);
    }

    /* Watch for SIGCHLD signals */
    SetSignalHandler(SIGCHLD, HandleSIGCHLD);

#endif

    /* Change directory to 'rundir' */
    if (Chdir(OMI_GetPath(ID_RUNDIR)) != 0)
    {
        err(ZT("failed to change directory to: %s"), 
            scs(OMI_GetPath(ID_RUNDIR)));
    }

#if defined(CONFIG_POSIX)
    /* Daemonize */
    if (s_opts.daemonize && Process_Daemonize() != 0)
    {
        err(ZT("failed to daemonize server process"));
    }
#endif

#if defined(CONFIG_POSIX)

    /* Create PID file */
    if ((pidfile = PIDFile_OpenWrite()) == -1)
    {
        fprintf(stderr, "Could not create pid file %s\n", OMI_GetPath(ID_PIDFILE));
        trace_CreatePIDFileFailed( scs(OMI_GetPath(ID_PIDFILE)) );

        // Need to let the world know. We may not have a functioning log system at this point
        // or know to look

        fprintf(stderr, "Cannot create PID file. omi server exiting\n");
        exit(1);
    }

#endif

    /* If ntlm cred file is in use, check permissions and set NTLM_USER_FILE env variable */

    char *ntlm_user_file = getenv("NTLM_USER_FILE");
    if (ntlm_user_file)
    {
        /* We do NOT accept the NTLM_USER_FILE environement variable for the server */
        trace_NtlmEnvIgnored(ntlm_user_file);
        unsetenv("NTLM_USER_FILE");
    }

    if (s_opts.ntlmCredFile && !s_opts.ignoreAuthentication)
    {
       if (!ValidateNtlmCredsFile(s_opts.ntlmCredFile))
       {
           trace_NtlmCredFileInvalid(s_opts.ntlmCredFile);
       }
    }

    if (s_opts.nonRoot == MI_TRUE)
    {
        int r;

        r = VerifyServiceAccount();
        if (r != 0)
        {
            err(ZT("invalid service account:  %T"), s_opts.serviceAccount);
        }

        if (s_opts.serviceAccount)
        {
            PAL_Free((void*)s_opts.serviceAccount);
        }

        r = _CreateSockFile(socketFile, PAL_MAX_PATH_SIZE, secretString, S_SECRET_STRING_LENGTH);
        if (r != 0)
        {
            err(ZT("failed to create socket file"));
        }

        InitializeNetwork();

        r = _GiveEnginePermissions();
        if (r != MI_RESULT_OK)
        {
            err(ZT("Failed to give engine permission to files"));
        }

        r = _StartEngine(engine_argc, engine_argv, socketFile, secretString);
        if (r != 0)
        {
            err(ZT("failed to start omi engine"));
        }
    }

    while (!s_data.terminated)
    {
        if (s_opts.nonRoot != MI_TRUE)
        {
            result = InitializeNetwork();
            if (result != MI_RESULT_OK)
            {
                err(ZT("Failed to initialize network"));
            }

            result = WsmanProtocolListen();
            if (result != MI_RESULT_OK)
            {
                err(ZT("Failed to initialize Wsman"));
            }

            result = BinaryProtocolListenFile(OMI_GetPath(ID_SOCKETFILE), &s_data.mux[0], &s_data.protocol0, NULL);
            if (result != MI_RESULT_OK)
            {
                err(ZT("Failed to initialize binary protocol for socket file"));
            }
        }

        result = RunProtocol();
        if (result != MI_RESULT_OK)
        {
            err(ZT("Failed protocol loop"));
        }
    }

    free(engine_argv);
    ServerCleanup(pidfile);
    _RevertFileOwnership();

    return 0;
}
