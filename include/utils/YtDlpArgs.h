#ifndef YTDLPARGS_H
#define YTDLPARGS_H

#include <QString>
#include <QStringList>
#include "utils/YtDlpJobSpec.h"

// Pure construction of the yt-dlp command line. Deliberately free of QObject,
// singletons and I/O so it can be unit-tested headlessly (no yt-dlp, ffmpeg or
// database required).
namespace YtDlpArgs {

// Build the full argv for one job (everything after the executable path).
//
// ffmpegPath is the ffmpeg binary Copper installed in its private tools
// directory, or empty when ffmpeg is unavailable. When set it is forwarded as
// `--ffmpeg-location`: without it yt-dlp cannot find the bundled ffmpeg and
// silently leaves bestvideo+bestaudio as two separate files instead of muxing
// them into one (the 0.4.1 regression this fixes).
QStringList build(const YtDlpJobSpec& spec, const QString& userAgent, const QString& ffmpegPath);

// Whether the given output format needs ffmpeg. Every format Copper offers
// (mp3 transcodes audio; mp4/mkv/best must mux separate streams) does. The
// `best`/`mkv` cases used to be reported as "not required" and then rejected
// outright, so selecting them never worked.
bool requiresFfmpeg(const QString& format);

}  // namespace YtDlpArgs

#endif  // YTDLPARGS_H
