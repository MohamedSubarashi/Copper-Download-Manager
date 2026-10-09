// Copper Download Manager - unit tests for the dependency-light utilities.
//
// Deliberately curated: only sources that build without the GUI stack, so the
// binary runs headless anywhere (CI, dev shells). Isolation mirrors the
// integration harness - QStandardPaths test mode plus per-run APPDATA /
// LOCALAPPDATA overrides - so a test run never touches the real profile.

#include <QtTest>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QDir>

#include "copper_version.h"
#include "utils/DependencyVerifier.h"
#include "utils/FileNameSanitizer.h"
#include "utils/UserAgent.h"
#include "utils/ApiToken.h"
#include "utils/UrlDetector.h"
#include "utils/YtDlpArgs.h"

class CopperUnitTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    // DependencyVerifier: the fail-closed contract for downloaded binaries.
    void sha256Hex_knownVector();
    void manifest_hasPins();
    void verifyPinned_failsClosedOnWrongUrl();
    void verifyPinned_failsClosedOnTamperedData();
    void verifyPinned_failsClosedWithoutPin();
    void publishedHash_parsesCoreutilsFormat();
    void publishedHash_absentFileIsEmpty();
    void verifyAgainstSumsText_acceptsMatchRejectsMismatch();

    // FileNameSanitizer: hostile names never become paths.
    void sanitizeFileName_stripsForbiddenCharacters();
    void sanitizeFileName_blocksPathTraversal();
    void sanitizeFileName_fallbacksAndLimits();

    // UrlDetector: intake classification (the 0.5.3 "vivid intake" contract).
    void urlDetector_classifiesIntakeUrls();

    // Version single-source (0.4.0): the UA derives from the generated header.
    void version_isThreePartSemver();
    void userAgent_followsApplicationVersion();

    // API token (0.4.0): 64-hex, stable, constant-time matching semantics.
    void apiToken_roundTrip();

    // yt-dlp arguments (0.4.1): ffmpeg must be locatable or the video and audio
    // are left as two separate files; the merge container must match the choice.
    void ytDlpArgs_passesFfmpegLocation();
    void ytDlpArgs_omitsFfmpegLocationWhenUnavailable();
    void ytDlpArgs_picksContainerForMergeFormats();
    void ytDlpArgs_allFormatsRequireFfmpeg();

private:
    QTemporaryDir m_profile;
};

void CopperUnitTests::initTestCase() {
    // Redirect every profile path into a per-run temp dir BEFORE the first
    // Logger/ApiToken use (they are lazy singletons).
    QVERIFY(m_profile.isValid());
    QStandardPaths::setTestModeEnabled(true);
    qputenv("APPDATA", m_profile.path().toUtf8() + "/appdata");
    qputenv("LOCALAPPDATA", m_profile.path().toUtf8() + "/localappdata");

    // Same wiring main.cpp applies: one version source (copper_version.h).
    QCoreApplication::setApplicationVersion(QStringLiteral(COPPER_VERSION_STRING));
}

// --- DependencyVerifier -----------------------------------------------------

void CopperUnitTests::sha256Hex_knownVector() {
    // FIPS 180-2 known-answer test for "abc".
    QCOMPARE(DependencyVerifier::sha256Hex(QByteArrayLiteral("abc")),
             QStringLiteral("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

void CopperUnitTests::manifest_hasPins() {
    QVERIFY2(DependencyVerifier::hasPin("aria2"), "aria2 pin missing from :/dependencies.json");
    // ffmpeg's entry is keyed per platform and dependencies.json ships a
    // windows pin only, so the assertion follows the platform.
#ifdef Q_OS_WIN
    QVERIFY2(DependencyVerifier::hasPin("ffmpeg"), "ffmpeg pin missing from :/dependencies.json");
#else
    QVERIFY(!DependencyVerifier::hasPin("ffmpeg"));
#endif
    QVERIFY(!DependencyVerifier::hasPin("no-such-tool"));
}

void CopperUnitTests::verifyPinned_failsClosedOnWrongUrl() {
    // The pin is tied to the URL: a download URL changed in code without a
    // matching manifest update must block the install, not skip verification.
    QString err;
    QVERIFY(!DependencyVerifier::verifyPinned("aria2", "https://evil.example/aria2.zip",
                                              QByteArrayLiteral("data"), &err));
    QVERIFY2(err.contains("manifest"), qPrintable(err));
}

void CopperUnitTests::verifyPinned_failsClosedOnTamperedData() {
    // Right URL, wrong bytes: hash mismatch blocks the install.
    QString err;
    const QByteArray url = "https://github.com/aria2/aria2/releases/download/"
                           "release-1.37.0/aria2-1.37.0-win-64bit-build1.zip";
    QVERIFY(!DependencyVerifier::verifyPinned("aria2", QString::fromUtf8(url),
                                              QByteArrayLiteral("not the real archive"), &err));
    QVERIFY2(err.contains("sha256"), qPrintable(err));
}

void CopperUnitTests::verifyPinned_failsClosedWithoutPin() {
    QString err;
    QVERIFY(!DependencyVerifier::verifyPinned("no-such-tool", "https://example.com/x.zip",
                                              QByteArrayLiteral("data"), &err));
    QVERIFY2(err.contains("refusing"), qPrintable(err));
}

void CopperUnitTests::publishedHash_parsesCoreutilsFormat() {
    const QByteArray sums =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  file.bin\n"
        "d2a84f4b8b650937ec8f73cd8be2c74add5a911ba64df27458ed8229da804a26  other.bin\n";
    QCOMPARE(DependencyVerifier::publishedHashFor(sums, "file.bin"),
             QStringLiteral("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    // CRLF-tolerant (published files vary).
    QCOMPARE(DependencyVerifier::publishedHashFor("abc123\r\n", QStringLiteral("x")), QString());
}

void CopperUnitTests::publishedHash_absentFileIsEmpty() {
    const QByteArray sums =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  file.bin\n";
    QVERIFY(DependencyVerifier::publishedHashFor(sums, "missing.bin").isEmpty());
    QVERIFY(DependencyVerifier::publishedHashFor(QByteArrayLiteral("garbage"), "file.bin").isEmpty());
    QVERIFY(DependencyVerifier::publishedHashFor(QByteArray(), "file.bin").isEmpty());
}

void CopperUnitTests::verifyAgainstSumsText_acceptsMatchRejectsMismatch() {
    const QByteArray data = QByteArrayLiteral("hello");
    const QByteArray sums = DependencyVerifier::sha256Hex(data).toLatin1() + "  hello.txt\n";
    QString err;
    QVERIFY2(DependencyVerifier::verifyAgainstSumsText(sums, "hello.txt", data, &err),
             qPrintable(err));

    QVERIFY(!DependencyVerifier::verifyAgainstSumsText(sums, "hello.txt",
                                                       QByteArrayLiteral("hellO"), &err));
    QVERIFY(!err.isEmpty());
    // Fail closed when the file simply is not listed.
    QVERIFY(!DependencyVerifier::verifyAgainstSumsText(sums, "other.txt", data, &err));
    QVERIFY(err.contains("refusing"));
}

// --- FileNameSanitizer ------------------------------------------------------

void CopperUnitTests::sanitizeFileName_stripsForbiddenCharacters() {
    QCOMPARE(sanitizeFileName("a<b>c.txt"), QStringLiteral("abc.txt"));
    QCOMPARE(sanitizeFileName("report: draft?.pdf"), QStringLiteral("report draft.pdf"));
    QCOMPARE(sanitizeFileName("trailing."), QStringLiteral("trailing"));
    QCOMPARE(sanitizeFileName("tab\tand\ncontrol\x01" "chars"), QStringLiteral("tabandcontrolchars"));
}

void CopperUnitTests::sanitizeFileName_blocksPathTraversal() {
    QCOMPARE(sanitizeFileName("..\\..\\evil.exe"), QStringLiteral("evil.exe"));
    QCOMPARE(sanitizeFileName("../../etc/passwd"), QStringLiteral("passwd"));
    QCOMPARE(sanitizeFileName(".."), QStringLiteral("download"));
    QCOMPARE(sanitizeFileName("."), QStringLiteral("download"));
}

void CopperUnitTests::sanitizeFileName_fallbacksAndLimits() {
    QCOMPARE(sanitizeFileName(""), QStringLiteral("download"));
    QCOMPARE(sanitizeFileName("..."), QStringLiteral("download"));
    // The fallback only replaces results that sanitize down to nothing.
    QCOMPARE(sanitizeFileName("", "custom.bin"), QStringLiteral("custom.bin"));
    QCOMPARE(sanitizeFileName("...", "custom.bin"), QStringLiteral("custom.bin"));
    QCOMPARE(sanitizeFileName("keep.txt", "custom.bin"), QStringLiteral("keep.txt"));
    QCOMPARE(sanitizeFileName(QString(250, 'a')).size(), 200);
}

// --- UrlDetector ------------------------------------------------------------

void CopperUnitTests::urlDetector_classifiesIntakeUrls() {
    QCOMPARE(UrlDetector::detect("magnet:?xt=urn:btih:0123456789abcdef"), UrlTorrent);
    QCOMPARE(UrlDetector::detect("https://example.com/files/x.torrent"), UrlTorrent);
    QCOMPARE(UrlDetector::detect("https://www.youtube.com/playlist?list=PL123"), UrlPlaylist);
    QCOMPARE(UrlDetector::detect("https://www.youtube.com/watch?v=abc&list=PL1"), UrlPlaylist);
    QCOMPARE(UrlDetector::detect("https://www.youtube.com/watch?v=abc"), UrlYtDlp);
    QCOMPARE(UrlDetector::detect("https://vimeo.com/12345"), UrlYtDlp);
    QCOMPARE(UrlDetector::detect("https://example.com/files/video.mp4"), UrlDirect);
    QCOMPARE(UrlDetector::detect("https://example.com/"), UrlUnknown);
    QCOMPARE(UrlDetector::detect("nonsense"), UrlUnknown);
    QCOMPARE(UrlDetector::typeToString(UrlYtDlp), QStringLiteral("Video (yt-dlp)"));
}

// --- Version single-source --------------------------------------------------

void CopperUnitTests::version_isThreePartSemver() {
    const QString v = QStringLiteral(COPPER_VERSION_STRING);
    QVERIFY2(QRegularExpression("^[0-9]+\\.[0-9]+\\.[0-9]+$").match(v).hasMatch(),
             qPrintable("COPPER_VERSION_STRING is not x.y.z: " + v));
}

void CopperUnitTests::userAgent_followsApplicationVersion() {
    // Prefix assembled separately from the version so this test asserts the
    // single-source wiring rather than hardcoding a UA string the version
    // linter would (rightly) reject.
    const QString prefix = QStringLiteral("CopperDownloadManager/");
    QCoreApplication::setApplicationVersion("9.9.9-test");
    QVERIFY(Copper::copperUserAgent().contains(prefix + "9.9.9-test"));
    QCOMPARE(Copper::bareUserAgent(), prefix + "9.9.9-test");
    QCoreApplication::setApplicationVersion(QStringLiteral(COPPER_VERSION_STRING));
    // After restoring, the UA must carry the single-source version - this is
    // what catches a stale hardcoded UA version suffix.
    QVERIFY(Copper::bareUserAgent().endsWith(QStringLiteral(COPPER_VERSION_STRING)));
}

// --- API token --------------------------------------------------------------

void CopperUnitTests::apiToken_roundTrip() {
    const QString token = ApiToken::token();
    QCOMPARE(token.size(), 64);
    QVERIFY2(QRegularExpression("^[0-9a-f]{64}$").match(token).hasMatch(),
             qPrintable("token is not 64 lowercase hex: " + token));
    QVERIFY(ApiToken::matches(token));
    QVERIFY(!ApiToken::matches(QString(64, '0')));
    QVERIFY(!ApiToken::matches(""));
    QVERIFY(!ApiToken::matches(token.left(63)));
    // Stable across calls (persisted + cached).
    QCOMPARE(ApiToken::token(), token);
}

// --- yt-dlp arguments (0.4.1) -----------------------------------------------

namespace {
// Value that follows `flag` in argv, or an empty string when the flag is absent.
QString flagValue(const QStringList& args, const QString& flag) {
    const int i = args.indexOf(flag);
    return i >= 0 ? args.value(i + 1) : QString();
}
}  // namespace

void CopperUnitTests::ytDlpArgs_passesFfmpegLocation() {
    // Regression: the bundled ffmpeg was never handed to yt-dlp, so yt-dlp could
    // not mux bestvideo+bestaudio and wrote the video and audio as two files.
    YtDlpJobSpec spec;
    spec.url = "https://www.youtube.com/watch?v=abc";
    spec.outputTemplate = "%(title)s.%(ext)s";
    spec.format = "mp4";
    const QString ffmpeg = "C:/Users/x/AppData/Roaming/Copper Download Manager/tools/ffmpeg.exe";
    const QStringList args = YtDlpArgs::build(spec, "Copper/1.0", ffmpeg);
    QCOMPARE(flagValue(args, "--ffmpeg-location"), ffmpeg);
    QCOMPARE(args.last(), spec.url);
    QCOMPARE(flagValue(args, "--merge-output-format"), QStringLiteral("mp4"));
}

void CopperUnitTests::ytDlpArgs_omitsFfmpegLocationWhenUnavailable() {
    YtDlpJobSpec spec;
    spec.url = "u";
    spec.outputTemplate = "t";
    spec.format = "mp4";
    const QStringList args = YtDlpArgs::build(spec, "UA", QString());
    QVERIFY(!args.contains("--ffmpeg-location"));
}

void CopperUnitTests::ytDlpArgs_picksContainerForMergeFormats() {
    YtDlpJobSpec spec;
    spec.url = "u";
    spec.outputTemplate = "t";

    spec.format = "mp4";
    QCOMPARE(flagValue(YtDlpArgs::build(spec, "UA", "f"), "--merge-output-format"),
             QStringLiteral("mp4"));
    // MKV used to be unmapped and silently became mp4.
    spec.format = "mkv";
    QCOMPARE(flagValue(YtDlpArgs::build(spec, "UA", "f"), "--merge-output-format"),
             QStringLiteral("mkv"));
    // mp3 asks yt-dlp to extract audio rather than merge a video container.
    spec.format = "mp3";
    {
        const QStringList args = YtDlpArgs::build(spec, "UA", "f");
        QVERIFY(args.contains("--extract-audio"));
        QCOMPARE(flagValue(args, "--audio-format"), QStringLiteral("mp3"));
        QVERIFY(!args.contains("--merge-output-format"));
    }
    // "best" lets yt-dlp choose the container.
    spec.format = "best";
    QVERIFY(!YtDlpArgs::build(spec, "UA", "f").contains("--merge-output-format"));
}

void CopperUnitTests::ytDlpArgs_allFormatsRequireFfmpeg() {
    // Regression: mkv/best were reported as not requiring ffmpeg and then
    // rejected outright, so those formats never worked even with ffmpeg present.
    QVERIFY(YtDlpArgs::requiresFfmpeg("mp3"));
    QVERIFY(YtDlpArgs::requiresFfmpeg("mp4"));
    QVERIFY(YtDlpArgs::requiresFfmpeg("mkv"));
    QVERIFY(YtDlpArgs::requiresFfmpeg("best"));
}

QTEST_MAIN(CopperUnitTests)
#include "unit_tests.moc"
