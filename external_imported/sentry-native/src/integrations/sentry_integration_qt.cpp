#include "sentry_integration_qt.h"
#include "sentry_boot.h"

#include <QtCore/qglobal.h>
#include <QtCore/qstring.h>

static QtMessageHandler originalMessageHandler = nullptr;

static const char *
logLevelForMessageType(QtMsgType msgType)
{
    switch (msgType) {
    case QtDebugMsg:
        return "debug";
    case QtWarningMsg:
        return "warning";
    case QtCriticalMsg:
        return "error";
    case QtFatalMsg:
        return "fatal";
    case QtInfoMsg:
        Q_FALLTHROUGH();
    default:
        return "info";
    }
}

static void
sentry_qt_messsage_handler(
    QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    sentry_value_t crumb
        = sentry_value_new_breadcrumb("default", qUtf8Printable(message));

    sentry_value_set_by_key(
        crumb, "category", sentry_value_new_string(context.category));
    sentry_value_set_by_key(
        crumb, "level", sentry_value_new_string(logLevelForMessageType(type)));

    sentry_value_t location = sentry_value_new_object();
    sentry_value_set_by_key(
        location, "file", sentry_value_new_string(context.file));
    sentry_value_set_by_key(
        location, "line", sentry_value_new_int32(context.line));
    sentry_value_set_by_key(crumb, "data", location);

    sentry_add_breadcrumb(crumb);

    // Don't interfere with normal logging, by forwarding
    // to any existing message handlers.
    if (originalMessageHandler)
        originalMessageHandler(type, context, message);
}

void
sentry_integration_setup_qt(void)
{
    originalMessageHandler = qInstallMessageHandler(sentry_qt_messsage_handler);
}
