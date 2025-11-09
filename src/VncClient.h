#pragma once

#include <QImage>
#include <QMutex>
#include <QThread>

struct _rfbClient;

class VncClient : public QThread
{
    Q_OBJECT

public:
    explicit VncClient(QObject *parent = nullptr);
    ~VncClient() override;

    bool startClient(const QString &host, int port);
    void stopClient();
    bool isRunning() const;

public slots:
    void postPointerEvent(int x, int y, int buttons);
    void postKeyEvent(quint32 keysym, bool pressed);

signals:
    void framebufferUpdated(const QImage &image);
    void disconnected(const QString &reason);

protected:
    void run() override;

private:
    static void handleFramebufferUpdate(struct _rfbClient *client, int x, int y, int w, int h);
    void processFramebuffer(struct _rfbClient *client);
    void emitFramebuffer();
    void cleanupClient();

    QString m_host;
    int m_port;
    struct _rfbClient *m_client;
    QMutex m_clientMutex;
    QMutex m_imageMutex;
    QImage m_framebuffer;
    bool m_running;
};
