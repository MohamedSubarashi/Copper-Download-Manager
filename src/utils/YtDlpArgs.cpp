#include "utils/YtDlpArgs.h"

namespace {

// Progress/print protocol shared with yt-dlp. A custom --progress-template
// replaces yt-dlp's default progress line, which is what lets a single playlist
// process report per-video numbers the table can attribute to individual rows.
//   COPPER|<state>|<downloaded>|<total>|<speed>|<eta>|<info.id>|<frag idx>|<frag count>
//   COPPERPATH|<playlist_index or -1>|<final path>
const char* kProgressTemplate =
    "download:COPPER|%(progress.status)s|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|"
    "%(progress.speed)s|%(progress.eta)s|%(info.id)s|%(progress.fragment_index)s|%(progress.fragment_count)s";

}  // namespace

namespace YtDlpArgs {

bool requiresFfmpeg(const QString& format) {
    Q_UNUSED(format);
    // mp3 -> FFmpegExtractAudio; mp4/mkv/best -> mux bestvideo+bestaudio.
    return true;
}

QStringList build(const YtDlpJobSpec& spec, const QString& userAgent, const QString& ffmpegPath) {
    QStringList args;
    args << "-o" << spec.outputTemplate;
    args << "--newline";
    args << "--no-warnings";
    args << "--progress";
    // Resume partial .part files instead of restarting the transfer.
    args << "--continue";
    args << "--user-agent" << userAgent;
#ifdef PLATFORM_WINDOWS
    // Match the app's own filename sanitizing so the file on disk is the file the
    // table (and the resume check on restart) expects.
    args << "--windows-filenames";
#endif
    // Hand yt-dlp the ffmpeg Copper installed itself. The tools dir is not on
    // PATH, so without this yt-dlp falls back to downloading the video and audio
    // streams separately and skips the mux step (exit 0, two files on disk).
    if (!ffmpegPath.isEmpty()) {
        args << "--ffmpeg-location" << ffmpegPath;
    }
    // Per-video progress + the final path of every finished file.
    args << "--progress-template" << kProgressTemplate;
    args << "--print" << (spec.isPlaylist
                              ? QString("after_move:COPPERPATH|%(playlist_index)s|%(filepath)s")
                              : QString("after_move:COPPERPATH|-1|%(filepath)s"));

    if (spec.isPlaylist) {
        // A single unavailable/removed item must not cancel the whole playlist,
        // and a bounded socket timeout keeps a dead TCP connection from hanging
        // the job forever.
        args << "--ignore-errors";
        args << "--no-abort-on-error";
        args << "--socket-timeout" << "30";
        if (spec.fragments > 1) {
            args << "-N" << QString::number(spec.fragments);
        }
        if (!spec.archivePath.isEmpty()) {
            // Resuming/restarting a job must not re-download items that already
            // finished, which is exactly what this archive makes yt-dlp skip.
            args << "--download-archive" << spec.archivePath;
        }
        if (!spec.selectedItems.isEmpty()) {
            // Only fetch the videos the user actually picked. Without this, pointing
            // yt-dlp at the playlist URL downloads every item and leaves files on
            // disk that no table row refers to.
            QStringList positions;
            for (int idx : spec.selectedItems) positions << QString::number(idx);
            args << "--playlist-items" << positions.join(",");
        }
    }

    if (spec.format == "mp3") {
        args << "--extract-audio";
        args << "--audio-format" << "mp3";
    } else if (spec.format == "mkv") {
        args << "--merge-output-format" << "mkv";
    } else if (spec.format != "best") {
        args << "--merge-output-format" << "mp4";
    }

    args << spec.url;
    return args;
}

}  // namespace YtDlpArgs
