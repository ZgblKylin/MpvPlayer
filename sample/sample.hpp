#ifndef MPVPLAYER_SAMPLE_SAMPLE_HPP
#define MPVPLAYER_SAMPLE_SAMPLE_HPP

#include <MpvPlayer.hpp>

class MpvPlayerQuickInput : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
  Q_PROPERTY(QUrl url READ url WRITE setUrl NOTIFY urlChanged)
  Q_PROPERTY(bool paused READ isPaused WRITE setPaused NOTIFY pausedChanged)

 public:
  explicit MpvPlayerQuickInput(const QString& name, const QUrl& url = QUrl(),
                               bool paused = false);

  QString name() const;
  Q_SLOT void setName(const QString& name);
  Q_SIGNAL void nameChanged(const QString& name);

  QUrl url() const;
  Q_SLOT void setUrl(const QUrl& url);
  Q_SIGNAL void urlChanged(const QUrl& url);

  bool isPaused() const;
  Q_SLOT void setPaused(bool paused);
  Q_SIGNAL void pausedChanged(bool paused);

 private:
  QString name_{};
  QUrl url_{};
  bool paused_ = false;
};

#endif  // MPVPLAYER_SAMPLE_SAMPLE_HPP
