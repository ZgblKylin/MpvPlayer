#ifndef MPVPLAYER_SAMPLE_SAMPLE_HPP
#define MPVPLAYER_SAMPLE_SAMPLE_HPP

#include <MpvPlayer.hpp>

class MpvPlayerQuickInput : public QObject {
  Q_OBJECT
  Q_PROPERTY(QUrl url READ url WRITE setUrl NOTIFY urlChanged)
  Q_PROPERTY(bool paused READ isPaused WRITE setPaused NOTIFY pausedChanged)

 public:
  explicit MpvPlayerQuickInput(const QUrl& url = QUrl(), bool paused = false);

  QUrl url() const;
  Q_SLOT void setUrl(const QUrl& url);
  Q_SIGNAL void urlChanged(const QUrl& url);

  bool isPaused() const;
  Q_SLOT void setPaused(bool paused);
  Q_SIGNAL void pausedChanged(bool paused);

 private:
  QUrl url_{};
  bool paused_ = false;
};

#endif  // MPVPLAYER_SAMPLE_SAMPLE_HPP
