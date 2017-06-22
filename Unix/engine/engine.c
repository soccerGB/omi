/*
**==============================================================================
**
** Copyright (c) Microsoft Corporation. All rights reserved. See file LICENSE
** for license information.
**
**==============================================================================
*/

#include <server/server.h>

static Options s_opts;
static ServerData s_data;

int enginemain(int argc, const char* argv[])
{
    int pidfile = -1;
    MI_Result result;

    SetDefaults(&s_opts, &s_data, argv[0], OMI_ENGINE);

    /* Get --destdir command-line option */
    GetCommandLineDestDirOption(&argc, argv);

    /* Extract configuration file options */
    GetConfigFileOptions();

    /* Extract command-line options a second time (to override) */
    GetCommandLineOptions(&argc, argv);

    /* Open the log file */
    OpenLogFile();

#if defined(CONFIG_POSIX)

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
        err(ZT("failed to change directory to: %s"), scs(OMI_GetPath(ID_RUNDIR)));
    }

#if defined(CONFIG_POSIX)
    /* Daemonize */
    if (s_opts.daemonize && Process_Daemonize() != 0)
    {
        err(ZT("failed to daemonize engine process"));
    }
#endif

    while (!s_data.terminated)
    {
        MI_Boolean r;

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

        // binary connection with server
        result = BinaryProtocolListenSock(s_opts.socketpairPort, &s_data.mux[1], &s_data.protocol1, NULL, NULL);
        if (result != MI_RESULT_OK)
        {
            err(ZT("Failed to initialize binary protocol for socket"));
        }

        r = SendSocketFileRequest(&s_data.protocol1->protocolSocket);
        if (r == MI_FALSE)
        {
            err(ZT("failed to send socket file request"));
        }

        // Give it a little time for SocketFile info to come back from server
        // Sleep_Milliseconds(50);
        
        // binary connection with client
        const char *path = OMI_GetPath(ID_SOCKETFILE);
        result = BinaryProtocolListenFile(path, &s_data.mux[0], &s_data.protocol0, NULL);
        if (result != MI_RESULT_OK)
        {
            err(ZT("Failed to initialize binary protocol for socket file"));
        }

        result = RunProtocol();
        if (result != MI_RESULT_OK)
        {
            err(ZT("Failed protocol loop"));
        }
    }

    ServerCleanup(pidfile);

    return 0;
}
