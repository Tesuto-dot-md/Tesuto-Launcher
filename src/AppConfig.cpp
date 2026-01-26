#include "AppConfig.h"
#include <QtCore>

AppConfig AppConfig::fromArgs(const QCoreApplication& app) {
    QCommandLineParser p;
    p.setApplicationDescription("Tesuto launcher");
    p.addHelpOption();

    QCommandLineOption oGameDir ("game-dir",    "Game directory",                          "dir",    "");
    QCommandLineOption oJava    ("java",        "Path to Java",                             "path",   "/usr/bin/java");
    QCommandLineOption oVer     ("version",     "Minecraft version",                        "ver",    "");
    QCommandLineOption oLogin   ("login",       "Run Microsoft login device flow");
    QCommandLineOption oMsClient("ms-client-id","Microsoft OAuth client id (fallback TESUTO_MS_CLIENT_ID)", "id", "");
    QCommandLineOption oMsTenant("ms-tenant",   "Azure AD tenant (default: consumers)",     "tenant", "consumers");
    QCommandLineOption oOffline ("offline-name","Offline player name (test only)",          "name",   "");

    p.addOption(oGameDir);
    p.addOption(oJava);
    p.addOption(oVer);
    p.addOption(oLogin);
    p.addOption(oMsClient);
    p.addOption(oMsTenant);
    p.addOption(oOffline);

    p.process(app);

    AppConfig c;
    c.gameDir    = p.value(oGameDir);
    c.javaPath   = p.value(oJava);
    c.version    = p.value(oVer);
    c.doLogin    = p.isSet(oLogin);
    c.msClientId = p.value(oMsClient);
    if (c.msClientId.isEmpty())
        c.msClientId = QString::fromUtf8(qgetenv("TESUTO_MS_CLIENT_ID"));
    c.msTenant   = p.value(oMsTenant);
    c.offlineName = p.value(oOffline);

    return c;
}
