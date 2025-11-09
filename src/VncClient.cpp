#include "VncClient.h"

#include <QByteArray>
#include <QMutexLocker>
#include <cstring>

namespace {
char kClientDataTag;
}

extern "C" {
#include <rfb/rfbclient.h>
}

VncClient::VncClient(QObject *parent)
    : QThread(parent)
    , m_port(0)
    , m_client(nullptr)
    , m_running(false)
{
    setObjectName(QStringLiteral("VncClientThread"));
}

VncClient::~VncClient()
{
    stopClient();
}

bool VncClient::startClient(const QString &host, int port)
{
    if (m_running) {
        return false;
    }
    m_host = host;
    m_port = port;
    m_running = true;
    start();
    return true;
}

void VncClient::stopClient()
{
    if (!m_running) {
        return;
    }
    requestInterruption();
    if (isRunning()) {
        wait(1000);
    }
    cleanupClient();
    m_running = false;
}

bool VncClient::isRunning() const
{
    return m_running;
}

void VncClient::postPointerEvent(int x, int y, int buttons)
{
    QMutexLocker locker(&m_clientMutex);
    if (m_client) {
        SendPointerEvent(m_client, x, y, buttons);
    }
}

void VncClient::postKeyEvent(quint32 keysym, bool pressed)
{
    QMutexLocker locker(&m_clientMutex);
    if (m_client) {
        SendKeyEvent(m_client, keysym, pressed);
    }
}

void VncClient::run()
{
    rfbClient *client = rfbGetClient(8, 3, 4);
    if (!client) {
        emit disconnected(QStringLiteral("Failed to create VNC client"));
        m_running = false;
        return;
    }
    {
        QMutexLocker locker(&m_clientMutex);
        m_client = client;
    }
    rfbClientSetClientData(client, &kClientDataTag, this);

    QByteArray hostUtf8 = m_host.toUtf8();
    client->serverHost = strdup(hostUtf8.constData());
    client->serverPort = m_port;
    client->programName = const_cast<char*>("qt-vm-launcher");

    client->format.bitsPerPixel = 32;
    client->format.depth = 24;
    client->format.bigEndian = 0;
    client->format.trueColour = 1;
    client->format.redShift = 16;
    client->format.greenShift = 8;
    client->format.blueShift = 0;
    client->format.redMax = 255;
    client->format.greenMax = 255;
    client->format.blueMax = 255;

    client->GotFrameBufferUpdate = &VncClient::handleFramebufferUpdate;
    client->FinishedFrameBufferUpdate = nullptr;

    if (!rfbInitClient(client, nullptr, nullptr)) {
        emit disconnected(QStringLiteral("Failed to initialize VNC connection"));
        cleanupClient();
        m_running = false;
        return;
    }

    while (!isInterruptionRequested() && client->sock >= 0) {
        int waitResult = WaitForMessage(client, 20);
        if (waitResult < 0) {
            break;
        }
        if (waitResult > 0) {
            if (!HandleRFBServerMessage(client)) {
                break;
            }
        }
    }

    emit disconnected(QStringLiteral("Connection closed"));
    cleanupClient();
    m_running = false;
}

void VncClient::handleFramebufferUpdate(rfbClient *client, int, int, int, int)
{
    VncClient *self = static_cast<VncClient*>(rfbClientGetClientData(client, &kClientDataTag));
    if (!self) {
        return;
    }
    self->processFramebuffer(client);
    self->emitFramebuffer();
}

void VncClient::processFramebuffer(rfbClient *client)
{
    QMutexLocker locker(&m_imageMutex);
    if (client->width <= 0 || client->height <= 0) {
        return;
    }
    if (m_framebuffer.size() != QSize(client->width, client->height)) {
        m_framebuffer = QImage(client->width, client->height, QImage::Format_RGB32);
    }

    const unsigned char *src = reinterpret_cast<const unsigned char*>(client->frameBuffer);
    for (int y = 0; y < client->height; ++y) {
        const quint32 *rowSrc = reinterpret_cast<const quint32*>(src + y * client->width * 4);
        quint32 *rowDst = reinterpret_cast<quint32*>(m_framebuffer.scanLine(y));
        for (int x = 0; x < client->width; ++x) {
            const quint32 px = rowSrc[x];
            const quint8 r = (px >> 16) & 0xFF;
            const quint8 g = (px >> 8) & 0xFF;
            const quint8 b = px & 0xFF;
            rowDst[x] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

void VncClient::emitFramebuffer()
{
    QImage imageCopy;
    {
        QMutexLocker locker(&m_imageMutex);
        imageCopy = m_framebuffer.copy();
    }
    if (!imageCopy.isNull()) {
        emit framebufferUpdated(imageCopy);
    }
}

void VncClient::cleanupClient()
{
    if (m_client) {
        QMutexLocker locker(&m_clientMutex);
        if (m_client) {
            rfbClientSetClientData(m_client, &kClientDataTag, nullptr);
            rfbClientCleanup(m_client);
            m_client = nullptr;
        }
    }
}
