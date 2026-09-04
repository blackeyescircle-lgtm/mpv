/*
 * mpv-grid: project model and independent auxiliary video pipelines.
 *
 * Tile 0 uses mpv's normal playback pipeline. Tiles 1..15 are decoded by
 * independent FFmpeg contexts in the same process and are composited by the
 * existing gpu-next VO. All auxiliary hardware decoders reference the device
 * exported by the single VO, so no extra window or swapchain is created.
 */
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/buffer.h>
#include <mpv/client.h>
#include <libswresample/swresample.h>

#include "mpv_talloc.h"
#include "common/common.h"
#include "common/msg.h"
#include "common/playlist.h"
#include "audio/chmap.h"
#include "audio/format.h"
#include "misc/io_utils.h"
#include "misc/json.h"
#include "misc/node.h"
#include "misc/path_utils.h"
#include "options/options.h"
#include "input/input.h"
#include "osdep/getpid.h"
#include "osdep/timer.h"
#include "osdep/threads.h"
#include "stream/stream.h"
#include "video/fmt-conversion.h"
#include "video/hwdec.h"
#include "video/img_format.h"
#include "video/image_writer.h"
#include "video/mp_image.h"
#include "video/mp_image_pool.h"
#include "video/out/vo.h"
#include "video/sws_utils.h"
#include "player/command.h"
#include "player/core.h"
#include "player/grid.h"

#if HAVE_WIN32_DESKTOP
#include "osdep/windows_utils.h"
#endif

#define GRID_FORMAT "mpv-grid-project"
#define GRID_VERSION 1
#define GRID_CONTAINER_MAGIC "MPVGRD1\0"
#define GRID_CONTAINER_MAGIC_SIZE 8
#define GRID_CONTAINER_TRAILER_SIZE 16
#define GRID_AUDIO_MAX_LAG 0.080
#define GRID_AUDIO_TARGET_LAG 0.040
#define GRID_SESSION_FORMAT "mpv-grid-session"
#define GRID_SESSION_VERSION 1

struct grid_item {
    char *path;
    char *absolute_hint;
    double fixed_start;
    double resume_position;
    double duration;
    // Keep the resume point that existed when a project was opened. The
    // five-second checkpoint may advance resume_position while the fixed F1
    // moment is being previewed, but F2 must retain its original destination.
    double opened_resume_position;
};

struct grid_decoder {
    struct mp_grid *grid;
    struct grid_tile *tile;
    mp_thread thread;
    bool thread_started;
    bool terminate;
    bool reload;
    bool seek_pending;
    double seek_target;
    int command_serial;

    AVFormatContext *format;
    AVCodecContext *codec;
    AVCodecContext *audio_codec;
    AVFrame *avframe;
    AVFrame *audio_frame;
    AVPacket *packet;
    int stream_index;
    int audio_stream_index;
    AVRational time_base;
    AVRational audio_time_base;
    SwrContext *swr;
    float *audio_ring;
    int audio_capacity;
    int audio_read;
    int audio_count;
    int audio_output_rate;
    double audio_resample_phase;
    double audio_read_position;
    bool audio_resync;
    uint64_t audio_preroll_dropped;
    uint64_t audio_sync_dropped;
    AVBufferRef *hw_device;
};

struct grid_tile {
    struct grid_item *items;
    int num_items;
    int current_index;
    bool paused;
    bool hidden;
    bool muted;
    bool subtitles;
    double volume;
    double speed;
    double position;
    double fixed_start;
    double clock_position;
    int64_t clock_ns;
    bool zoom_active;
    double center_x;
    double center_y;
    char *error;
    struct mp_image *frame;
    struct grid_decoder decoder;
    uint64_t audio_mixed_samples;
};

struct mp_grid {
    struct MPContext *mpctx;
    struct mp_log *log;
    mp_mutex lock;
    mp_cond wakeup;
    bool enabled;
    bool shutting_down;
    bool media_active;
    bool single_source_auto;
    bool automatic_mode;
    bool session_restore_checked;
    bool session_invalidated;
    double single_source_duration;
    int rows;
    int columns;
    int aspect_w;
    int aspect_h;
    int active_tile;
    enum mp_grid_open_mode open_mode;
    bool solo_active;
    bool solo_saved[MP_GRID_MAX_TILES];
    int audio_fifo[MP_GRID_MAX_TILES];
    int audio_fifo_count;
    int audio_fifo_limit;
    char *project_path;
    bool playback_snapshot;
    int64_t last_autosave_ns;
    int64_t last_session_save_ns;
    int original_loop_times;
    bool loop_override_active;
    struct vo *vo;
    AVBufferRef *shared_hw_device;
    struct grid_tile tiles[MP_GRID_MAX_TILES];
};

static int layout_count(struct mp_grid *grid)
{
    return grid->rows * grid->columns;
}

static bool audio_fifo_contains_locked(struct mp_grid *grid, int tile)
{
    for (int i = 0; i < grid->audio_fifo_count; i++) {
        if (grid->audio_fifo[i] == tile)
            return true;
    }
    return false;
}

static void audio_fifo_reconcile_locked(struct mp_grid *grid, int count);

static void audio_fifo_touch_locked(struct mp_grid *grid, int tile)
{
    audio_fifo_reconcile_locked(grid, layout_count(grid));
    int found = -1;
    for (int i = 0; i < grid->audio_fifo_count; i++) {
        if (grid->audio_fifo[i] == tile) {
            found = i;
            break;
        }
    }
    if (found >= 0) {
        for (int i = found; i + 1 < grid->audio_fifo_count; i++)
            grid->audio_fifo[i] = grid->audio_fifo[i + 1];
        grid->audio_fifo_count--;
    } else if (grid->audio_fifo_count >= grid->audio_fifo_limit) {
        for (int i = 0; i + 1 < grid->audio_fifo_count; i++)
            grid->audio_fifo[i] = grid->audio_fifo[i + 1];
        grid->audio_fifo_count--;
    }
    grid->audio_fifo[grid->audio_fifo_count++] = tile;
}

static void audio_fifo_reconcile_locked(struct mp_grid *grid, int count)
{
    int limit = MPCLAMP(grid->mpctx->opts->grid_audio_count, 1,
                        MP_GRID_MAX_TILES);
    int previous[MP_GRID_MAX_TILES];
    int previous_count = grid->audio_fifo_count;
    memcpy(previous, grid->audio_fifo, sizeof(previous));
    int out = 0;
    for (int i = 0; i < previous_count; i++) {
        int tile = previous[i];
        bool duplicate = false;
        for (int n = 0; n < out; n++)
            duplicate |= grid->audio_fifo[n] == tile;
        if (tile >= 0 && tile < count && !duplicate)
            grid->audio_fifo[out++] = tile;
    }
    if (out > limit) {
        int remove = out - limit;
        memmove(grid->audio_fifo, grid->audio_fifo + remove,
                limit * sizeof(grid->audio_fifo[0]));
        out = limit;
    }
    grid->audio_fifo_count = out;
    grid->audio_fifo_limit = limit;
    for (int tile = 0; tile < count && out < limit; tile++) {
        if (!audio_fifo_contains_locked(grid, tile))
            grid->audio_fifo[out++] = tile;
    }
    grid->audio_fifo_count = out;
}

static void audio_fifo_reconcile(struct mp_grid *grid, int count)
{
    mp_mutex_lock(&grid->lock);
    audio_fifo_reconcile_locked(grid, count);
    mp_mutex_unlock(&grid->lock);
}

static bool valid_layout(int rows, int columns)
{
    return (rows == 2 && columns == 2) ||
           (rows == 2 && columns == 3) ||
           (rows == 3 && columns == 3) ||
           (rows == 3 && columns == 4) ||
           (rows == 4 && columns == 4);
}

static bool parse_layout(const char *s, int *rows, int *columns)
{
    if (!s || !s[0] || strcmp(s, "no") == 0)
        return false;
    int r = 0, c = 0, used = 0;
    if (sscanf(s, "%dx%d%n", &r, &c, &used) != 2 || s[used] ||
        !valid_layout(r, c))
        return false;
    *rows = r;
    *columns = c;
    return true;
}

static char *settings_path(void *parent)
{
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0])
        return NULL;
    return mp_path_join(parent, base, "mpv-grid/layout.txt");
}

static void save_last_layout(struct mp_grid *grid)
{
    char *path = settings_path(NULL);
    if (!path)
        return;
    bstr dir = mp_dirname(path);
    char *dir0 = bstrto0(NULL, dir);
    mp_mkdirp(dir0);
    talloc_free(dir0);
    char value[16];
    int len = grid->enabled
                ? snprintf(value, sizeof(value), "%dx%d\n", grid->rows,
                           grid->columns)
                : snprintf(value, sizeof(value), "no\n");
    mp_save_to_file(path, value, len);
    talloc_free(path);
}

static bool load_last_layout(int *rows, int *columns, bool *enabled)
{
    char *path = settings_path(NULL);
    if (!path)
        return false;
    FILE *file = fopen(path, "rb");
    talloc_free(path);
    if (!file)
        return false;
    char value[16] = {0};
    bool ok = fread(value, 1, sizeof(value) - 1, file) > 0;
    fclose(file);
    value[strcspn(value, "\r\n")] = '\0';
    if (!ok)
        return false;
    if (!strcmp(value, "no")) {
        *enabled = false;
        return true;
    }
    *enabled = true;
    return parse_layout(value, rows, columns);
}

static char *session_path(void *parent)
{
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0])
        return NULL;
    return mp_path_join(parent, base, "mpv-grid/last-session.json");
}

static void apply_grid_looping(struct mp_grid *grid)
{
    struct MPOpts *opts = grid->mpctx->opts;
    if (!grid->loop_override_active) {
        grid->original_loop_times = opts->loop_times;
        grid->loop_override_active = true;
    }
    if (opts->play_frames < 0)
        opts->loop_times = -1;
}

static void restore_normal_looping(struct mp_grid *grid)
{
    if (!grid->loop_override_active)
        return;
    grid->mpctx->opts->loop_times = grid->original_loop_times;
    grid->loop_override_active = false;
}

static bool load_last_session(struct mp_grid *grid, const char *filename,
                              bool *enabled, int *rows, int *columns,
                              double positions[MP_GRID_MAX_TILES],
                              int *num_positions);
static void save_last_session(struct mp_grid *grid, bool eof);
static void invalidate_last_session(struct mp_grid *grid);

static void set_error(struct grid_tile *tile, const char *text)
{
    talloc_free(tile->error);
    tile->error = text ? talloc_strdup(NULL, text) : NULL;
}

static void decoder_set_error(struct mp_grid *grid, struct grid_tile *tile,
                              const char *text)
{
    mp_mutex_lock(&grid->lock);
    set_error(tile, text);
    mp_mutex_unlock(&grid->lock);
}

static struct grid_item *current_item(struct grid_tile *tile)
{
    if (tile->current_index < 0 || tile->current_index >= tile->num_items)
        return NULL;
    return &tile->items[tile->current_index];
}

static double tile_clock_locked(struct grid_tile *tile, int64_t now)
{
    if (tile->paused || tile->hidden || tile->clock_ns <= 0)
        return tile->clock_position;
    return tile->clock_position + MP_TIME_NS_TO_S(now - tile->clock_ns) * tile->speed;
}

static void set_tile_clock_locked(struct grid_tile *tile, double position, int64_t now)
{
    tile->position = position;
    tile->clock_position = position;
    tile->clock_ns = now;
}

static enum AVPixelFormat grid_get_format(AVCodecContext *avctx,
                                          const enum AVPixelFormat *fmts)
{
    struct grid_decoder *decoder = avctx->opaque;
    if (decoder && decoder->hw_device) {
        for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; i++)
            if (fmts[i] == AV_PIX_FMT_D3D11)
                return fmts[i];
    }
    return fmts[0];
}

static void decoder_close(struct grid_decoder *decoder)
{
    av_packet_free(&decoder->packet);
    av_frame_free(&decoder->avframe);
    av_frame_free(&decoder->audio_frame);
    avcodec_free_context(&decoder->codec);
    avcodec_free_context(&decoder->audio_codec);
    swr_free(&decoder->swr);
    avformat_close_input(&decoder->format);
    decoder->stream_index = -1;
    decoder->audio_stream_index = -1;
}

static bool open_audio_decoder(struct mp_grid *grid, struct grid_tile *tile,
                               AVFormatContext *format)
{
    struct grid_decoder *decoder = &tile->decoder;
    decoder->audio_stream_index = av_find_best_stream(format,
        AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (decoder->audio_stream_index < 0)
        return true;

    AVStream *stream = format->streams[decoder->audio_stream_index];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    decoder->audio_codec = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!decoder->audio_codec ||
        avcodec_parameters_to_context(decoder->audio_codec, stream->codecpar) < 0)
    {
        avcodec_free_context(&decoder->audio_codec);
        decoder->audio_stream_index = -1;
        MP_WARN(grid, "Grid tile audio decoder unavailable; video will continue.\n");
        return true;
    }
    decoder->audio_time_base = stream->time_base;
    decoder->audio_codec->pkt_timebase = stream->time_base;
    if (avcodec_open2(decoder->audio_codec, codec, NULL) < 0) {
        avcodec_free_context(&decoder->audio_codec);
        decoder->audio_stream_index = -1;
        MP_WARN(grid, "Grid tile audio decoder unavailable; video will continue.\n");
        return true;
    }

    AVChannelLayout input_layout = decoder->audio_codec->ch_layout;
    if (!input_layout.nb_channels)
        av_channel_layout_default(&input_layout, 2);
    AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
    double speed = MPCLAMP(tile->speed, 0.1, 8.0);
    int converted_rate = MPCLAMP((int)llrint(48000.0 / speed), 6000, 192000);
    if (swr_alloc_set_opts2(&decoder->swr, &output_layout, AV_SAMPLE_FMT_FLT,
                            converted_rate, &input_layout,
                            decoder->audio_codec->sample_fmt,
                            decoder->audio_codec->sample_rate, 0, NULL) < 0 ||
        swr_init(decoder->swr) < 0)
    {
        swr_free(&decoder->swr);
        avcodec_free_context(&decoder->audio_codec);
        decoder->audio_stream_index = -1;
        MP_WARN(grid, "Grid tile audio resampler unavailable; video will continue.\n");
        return true;
    }
    decoder->audio_frame = av_frame_alloc();
    if (!decoder->audio_frame)
        return false;
    decoder->audio_capacity = 48000 * 4;
    decoder->audio_ring = talloc_realloc(grid, decoder->audio_ring, float,
                                         decoder->audio_capacity * 2);
    decoder->audio_read = decoder->audio_count = 0;
    decoder->audio_output_rate = converted_rate;
    decoder->audio_resample_phase = 0;
    decoder->audio_read_position = 0;
    decoder->audio_resync = true;
    decoder->audio_preroll_dropped = 0;
    decoder->audio_sync_dropped = 0;
    return decoder->audio_ring != NULL;
}

static bool decoder_open(struct mp_grid *grid, struct grid_tile *tile,
                         const char *path)
{
    struct grid_decoder *decoder = &tile->decoder;
    decoder_close(decoder);

    if (avformat_open_input(&decoder->format, path, NULL, NULL) < 0) {
        decoder_set_error(grid, tile, "cannot open media");
        return false;
    }
    if (avformat_find_stream_info(decoder->format, NULL) < 0) {
        decoder_set_error(grid, tile, "cannot read stream information");
        decoder_close(decoder);
        return false;
    }
    if (decoder->format->duration > 0 &&
        decoder->format->duration != AV_NOPTS_VALUE)
    {
        mp_mutex_lock(&grid->lock);
        struct grid_item *item = current_item(tile);
        if (item && item->path && !strcmp(item->path, path))
            item->duration = decoder->format->duration / (double)AV_TIME_BASE;
        mp_mutex_unlock(&grid->lock);
    }

    decoder->stream_index = av_find_best_stream(decoder->format,
        AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (decoder->stream_index < 0) {
        decoder_set_error(grid, tile, "no video stream");
        decoder_close(decoder);
        return false;
    }

    AVStream *stream = decoder->format->streams[decoder->stream_index];
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec || !(decoder->codec = avcodec_alloc_context3(codec)) ||
        avcodec_parameters_to_context(decoder->codec, stream->codecpar) < 0)
    {
        decoder_set_error(grid, tile, "cannot create video decoder");
        decoder_close(decoder);
        return false;
    }

    decoder->time_base = stream->time_base;
    decoder->codec->pkt_timebase = stream->time_base;
    decoder->codec->opaque = decoder;
    decoder->codec->thread_count = 1;
    if (decoder->hw_device) {
        decoder->codec->hw_device_ctx = av_buffer_ref(decoder->hw_device);
        decoder->codec->get_format = grid_get_format;
    }

    if (avcodec_open2(decoder->codec, codec, NULL) < 0) {
        // A driver can reject excess hardware sessions. Retry in software.
        avcodec_free_context(&decoder->codec);
        decoder->codec = avcodec_alloc_context3(codec);
        if (!decoder->codec ||
            avcodec_parameters_to_context(decoder->codec, stream->codecpar) < 0 ||
            avcodec_open2(decoder->codec, codec, NULL) < 0)
        {
            decoder_set_error(grid, tile, "cannot open video decoder");
            decoder_close(decoder);
            return false;
        }
        MP_WARN(grid, "Grid tile hardware decode unavailable for %s; using software.\n", path);
    }

    decoder->avframe = av_frame_alloc();
    decoder->packet = av_packet_alloc();
    if (!decoder->avframe || !decoder->packet) {
        decoder_set_error(grid, tile, "out of memory");
        decoder_close(decoder);
        return false;
    }
    if (!open_audio_decoder(grid, tile, decoder->format)) {
        decoder_set_error(grid, tile, "out of memory");
        decoder_close(decoder);
        return false;
    }
    decoder_set_error(grid, tile, NULL);
    return true;
}

static void decoder_seek(struct grid_decoder *decoder, double seconds)
{
    if (!decoder->format || decoder->stream_index < 0)
        return;
    int64_t ts = av_rescale_q((int64_t)llrint(seconds * AV_TIME_BASE),
                              AV_TIME_BASE_Q, decoder->time_base);
    avformat_seek_file(decoder->format, decoder->stream_index,
                       INT64_MIN, ts, INT64_MAX, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(decoder->codec);
    if (decoder->audio_codec)
        avcodec_flush_buffers(decoder->audio_codec);
    mp_mutex_lock(&decoder->grid->lock);
    decoder->audio_read = decoder->audio_count = 0;
    decoder->audio_resample_phase = 0;
    decoder->audio_read_position = seconds;
    decoder->audio_resync = true;
    mp_mutex_unlock(&decoder->grid->lock);
}

static void decoder_queue_audio(struct mp_grid *grid, struct grid_decoder *decoder)
{
    while (decoder->audio_codec &&
           avcodec_receive_frame(decoder->audio_codec, decoder->audio_frame) >= 0)
    {
        int64_t timestamp = decoder->audio_frame->best_effort_timestamp;
        double frame_position = timestamp == AV_NOPTS_VALUE
                                    ? MP_NOPTS_VALUE
                                    : timestamp * av_q2d(decoder->audio_time_base);
        int out_samples = swr_get_out_samples(decoder->swr,
                                              decoder->audio_frame->nb_samples);
        float *converted = talloc_array(NULL, float, out_samples * 2);
        if (!converted) {
            av_frame_unref(decoder->audio_frame);
            return;
        }
        uint8_t *out[] = {(uint8_t *)converted};
        int written = swr_convert(decoder->swr, out, out_samples,
            (const uint8_t **)decoder->audio_frame->extended_data,
            decoder->audio_frame->nb_samples);
        av_frame_unref(decoder->audio_frame);
        if (written > 0) {
            mp_mutex_lock(&grid->lock);
            int skip = 0;
            if (decoder->audio_resync && frame_position != MP_NOPTS_VALUE) {
                double target = tile_clock_locked(decoder->tile, mp_time_ns());
                skip = MPCLAMP((int)ceil((target - frame_position) *
                                         decoder->audio_output_rate),
                               0, written);
                decoder->audio_preroll_dropped += skip;
                if (skip < written) {
                    decoder->audio_read_position = frame_position +
                        (double)skip / decoder->audio_output_rate;
                    decoder->audio_resync = false;
                }
            } else if (decoder->audio_resync) {
                decoder->audio_resync = false;
            }
            if (skip == written) {
                mp_mutex_unlock(&grid->lock);
                talloc_free(converted);
                continue;
            }
            int space = decoder->audio_capacity - decoder->audio_count;
            written = MPMIN(written - skip, space);
            int write = (decoder->audio_read + decoder->audio_count) %
                        decoder->audio_capacity;
            for (int i = 0; i < written; i++) {
                int dst = (write + i) % decoder->audio_capacity;
                decoder->audio_ring[dst * 2 + 0] = converted[(skip + i) * 2 + 0];
                decoder->audio_ring[dst * 2 + 1] = converted[(skip + i) * 2 + 1];
            }
            decoder->audio_count += written;
            mp_mutex_unlock(&grid->lock);
        }
        talloc_free(converted);
    }
}

static bool decoder_wait_until(struct mp_grid *grid, struct grid_tile *tile,
                               int serial, int64_t target)
{
    mp_mutex_lock(&grid->lock);
    while (!tile->decoder.terminate && tile->decoder.command_serial == serial &&
           !tile->paused && !tile->hidden && mp_time_ns() < target)
    {
        mp_cond_timedwait_until(&grid->wakeup, &grid->lock, target);
    }
    bool ok = !tile->decoder.terminate && tile->decoder.command_serial == serial;
    mp_mutex_unlock(&grid->lock);
    return ok;
}

static MP_THREAD_VOID decoder_thread(void *ptr)
{
    struct grid_decoder *decoder = ptr;
    struct grid_tile *tile = decoder->tile;
    struct mp_grid *grid = decoder->grid;
    mp_thread_set_name("grid/decoder");

    while (1) {
        mp_mutex_lock(&grid->lock);
        while (!decoder->terminate &&
               (tile->hidden || tile->paused || !current_item(tile)))
            mp_cond_wait(&grid->wakeup, &grid->lock);
        if (decoder->terminate) {
            mp_mutex_unlock(&grid->lock);
            break;
        }
        struct grid_item *item = current_item(tile);
        char *path = talloc_strdup(NULL, item->path);
        double start = decoder->seek_pending ? decoder->seek_target : tile->position;
        decoder->seek_pending = false;
        decoder->reload = false;
        int serial = decoder->command_serial;
        mp_mutex_unlock(&grid->lock);

        if (!decoder_open(grid, tile, path)) {
            talloc_free(path);
            mp_sleep_ns(MP_TIME_MS_TO_NS(250));
            continue;
        }
        decoder_seek(decoder, start);

        mp_mutex_lock(&grid->lock);
        set_tile_clock_locked(tile, start, mp_time_ns());
        mp_mutex_unlock(&grid->lock);

        bool reopen = false;
        while (!reopen) {
            mp_mutex_lock(&grid->lock);
            if (decoder->terminate || decoder->command_serial != serial ||
                decoder->reload || decoder->seek_pending)
            {
                reopen = true;
            }
            bool blocked = tile->paused || tile->hidden;
            mp_mutex_unlock(&grid->lock);
            if (reopen)
                break;
            if (blocked) {
                mp_sleep_ns(MP_TIME_MS_TO_NS(10));
                continue;
            }

            int rr = av_read_frame(decoder->format, decoder->packet);
            if (rr < 0) {
                avcodec_send_packet(decoder->codec, NULL);
            } else if (decoder->packet->stream_index == decoder->stream_index) {
                avcodec_send_packet(decoder->codec, decoder->packet);
            } else if (decoder->audio_codec &&
                       decoder->packet->stream_index == decoder->audio_stream_index)
            {
                avcodec_send_packet(decoder->audio_codec, decoder->packet);
                decoder_queue_audio(grid, decoder);
            }
            av_packet_unref(decoder->packet);

            bool got_frame = false;
            while (avcodec_receive_frame(decoder->codec, decoder->avframe) >= 0) {
                got_frame = true;
                int64_t best = decoder->avframe->best_effort_timestamp;
                double pts = best == AV_NOPTS_VALUE ? start : best * av_q2d(decoder->time_base);

                mp_mutex_lock(&grid->lock);
                double speed = tile->speed;
                int64_t target = tile->clock_ns +
                    MP_TIME_S_TO_NS((pts - tile->clock_position) / speed);
                mp_mutex_unlock(&grid->lock);
                if (!decoder_wait_until(grid, tile, serial, target)) {
                    av_frame_unref(decoder->avframe);
                    reopen = true;
                    break;
                }

                struct mp_image *image = mp_image_from_av_frame(decoder->avframe);
                av_frame_unref(decoder->avframe);
                if (!image)
                    continue;
                image->pts = pts;

                mp_mutex_lock(&grid->lock);
                talloc_free(tile->frame);
                tile->frame = image;
                tile->position = pts;
                mp_mutex_unlock(&grid->lock);
                mp_grid_request_redraw(grid);
            }

            if (rr < 0 && !got_frame) {
                mp_mutex_lock(&grid->lock);
                struct grid_item *cur = current_item(tile);
                if (tile->num_items > 1)
                    tile->current_index = (tile->current_index + 1) % tile->num_items;
                cur = current_item(tile);
                double next = cur ? cur->fixed_start : 0;
                set_tile_clock_locked(tile, next, mp_time_ns());
                decoder->command_serial++;
                decoder->reload = true;
                mp_mutex_unlock(&grid->lock);
                reopen = true;
            }
        }

        decoder_close(decoder);
        talloc_free(path);
    }

    decoder_close(decoder);
    MP_THREAD_RETURN();
}

static void start_decoder_locked(struct mp_grid *grid, int index)
{
    if (index <= 0 || index >= MP_GRID_MAX_TILES)
        return;
    struct grid_tile *tile = &grid->tiles[index];
    if (tile->decoder.thread_started)
        return;
    tile->decoder.hw_device = grid->shared_hw_device
        ? av_buffer_ref(grid->shared_hw_device) : NULL;
    tile->decoder.grid = grid;
    tile->decoder.tile = tile;
    if (mp_thread_create(&tile->decoder.thread, decoder_thread, &tile->decoder) == 0)
        tile->decoder.thread_started = true;
    else
        set_error(tile, "cannot start decoder thread");
}

struct mp_grid *mp_grid_create(struct MPContext *mpctx)
{
    struct mp_grid *grid = talloc_zero(mpctx, struct mp_grid);
    grid->mpctx = mpctx;
    grid->log = mp_log_new(grid, mpctx->log, "grid");
    grid->rows = grid->columns = 2;
    grid->aspect_w = 16;
    grid->aspect_h = 9;
    grid->open_mode = MP_GRID_OPEN_ASK;
    grid->audio_fifo_limit = MPCLAMP(mpctx->opts->grid_audio_count, 1,
                                     MP_GRID_MAX_TILES);
    grid->audio_fifo_count = MPMIN(grid->audio_fifo_limit, layout_count(grid));
    for (int i = 0; i < grid->audio_fifo_count; i++)
        grid->audio_fifo[i] = i;
    mp_mutex_init(&grid->lock);
    mp_cond_init(&grid->wakeup);
    for (int i = 0; i < MP_GRID_MAX_TILES; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        tile->current_index = -1;
        tile->volume = 1.0;
        tile->speed = 1.0;
        tile->center_x = tile->center_y = 0.5;
        tile->decoder.stream_index = -1;
    }
    return grid;
}

void mp_grid_prepare_destroy(struct mp_grid *grid)
{
    if (!grid)
        return;
    mp_mutex_lock(&grid->lock);
    grid->shutting_down = true;
    grid->vo = NULL;
    for (int i = 1; i < MP_GRID_MAX_TILES; i++) {
        grid->tiles[i].decoder.terminate = true;
        grid->tiles[i].decoder.command_serial++;
    }
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);
    for (int i = 1; i < MP_GRID_MAX_TILES; i++) {
        struct grid_decoder *decoder = &grid->tiles[i].decoder;
        if (decoder->thread_started) {
            mp_thread_join(decoder->thread);
            decoder->thread_started = false;
        }
    }
}

void mp_grid_destroy(struct mp_grid *grid)
{
    if (!grid)
        return;
    mp_grid_prepare_destroy(grid);
    for (int i = 1; i < MP_GRID_MAX_TILES; i++) {
        struct grid_decoder *decoder = &grid->tiles[i].decoder;
        av_buffer_unref(&decoder->hw_device);
        talloc_free(grid->tiles[i].frame);
    }
    av_buffer_unref(&grid->shared_hw_device);
    mp_cond_destroy(&grid->wakeup);
    mp_mutex_destroy(&grid->lock);
    talloc_free(grid);
}

bool mp_grid_configure(struct mp_grid *grid)
{
    struct MPOpts *opts = grid->mpctx->opts;
    int rows = 0, columns = 0;
    grid->original_loop_times = opts->loop_times;
    if (opts->grid_open_mode && strcmp(opts->grid_open_mode, "ask") &&
        strcmp(opts->grid_open_mode, "fixed") &&
        strcmp(opts->grid_open_mode, "resume"))
    {
        MP_ERR(grid, "Invalid --grid-open-mode value: %s\n", opts->grid_open_mode);
        return false;
    }
    // The Grid object is created before configuration files are parsed. Reset
    // the initial selection here so a configured limit always starts with
    // tiles 1..N instead of retaining the tail of the compiled default FIFO.
    mp_mutex_lock(&grid->lock);
    grid->audio_fifo_count = 0;
    mp_mutex_unlock(&grid->lock);
    if (opts->grid_project && opts->grid_project[0]) {
        if (!mp_grid_load_project(grid, opts->grid_project))
            return false;
        audio_fifo_reconcile(grid, grid->enabled ? layout_count(grid) : 1);
        if (opts->grid_open_mode && strcmp(opts->grid_open_mode, "ask")) {
            grid->open_mode = !strcmp(opts->grid_open_mode, "resume")
                                ? MP_GRID_OPEN_RESUME : MP_GRID_OPEN_FIXED;
            mp_mutex_lock(&grid->lock);
            for (int i = 0; i < MP_GRID_MAX_TILES; i++) {
                struct grid_item *item = current_item(&grid->tiles[i]);
                if (item) {
                    double position = grid->open_mode == MP_GRID_OPEN_RESUME
                                        ? item->resume_position : item->fixed_start;
                    set_tile_clock_locked(&grid->tiles[i], position, mp_time_ns());
                }
            }
            mp_mutex_unlock(&grid->lock);
        }
        if (!grid->enabled)
            return true;
        opts->force_srate = 48000;
        opts->audio_output_format = AF_FORMAT_FLOAT;
        // Grid queues loop by design. Keep finite --frames useful for
        // diagnostics, screenshots, and automated tests instead of turning it
        // into an endless playlist loop.
        apply_grid_looping(grid);
        return true;
    }
    bool automatic = opts->grid_layout && !strcmp(opts->grid_layout, "auto");
    grid->automatic_mode = automatic;
    bool restored_enabled = true;
    if (automatic && !load_last_layout(&rows, &columns, &restored_enabled)) {
        rows = columns = 2;
        restored_enabled = true;
    }
    if (automatic && !restored_enabled) {
        grid->enabled = false;
        audio_fifo_reconcile(grid, 1);
        return true;
    }
    bool normal_mode = opts->grid_layout &&
                       (!strcmp(opts->grid_layout, "no") ||
                        !strcmp(opts->grid_layout, "1x1"));
    if (!automatic && normal_mode) {
        grid->enabled = false;
        audio_fifo_reconcile(grid, 1);
        if (!strcmp(opts->grid_layout, "1x1"))
            save_last_layout(grid);
        return true;
    }
    if (!automatic && !parse_layout(opts->grid_layout, &rows, &columns)) {
        grid->enabled = false;
        audio_fifo_reconcile(grid, 1);
        return !opts->grid_layout;
    }
    grid->enabled = true;
    grid->rows = rows;
    grid->columns = columns;
    audio_fifo_reconcile(grid, layout_count(grid));
    save_last_layout(grid);
    opts->force_srate = 48000;
    opts->audio_output_format = AF_FORMAT_FLOAT;
    apply_grid_looping(grid);
    if (opts->grid_open_mode) {
        grid->open_mode = !strcmp(opts->grid_open_mode, "resume") ? MP_GRID_OPEN_RESUME :
                          !strcmp(opts->grid_open_mode, "fixed") ? MP_GRID_OPEN_FIXED :
                          MP_GRID_OPEN_ASK;
    }
    return true;
}

char *mp_grid_main_path(struct mp_grid *grid, void *ta_parent)
{
    if (!grid || (!grid->enabled && !grid->project_path))
        return NULL;
    mp_mutex_lock(&grid->lock);
    struct grid_item *item = current_item(&grid->tiles[0]);
    char *path = item ? talloc_strdup(ta_parent, item->path) : NULL;
    mp_mutex_unlock(&grid->lock);
    return path;
}

bool mp_grid_populate_main_playlist(struct mp_grid *grid, struct playlist *playlist)
{
    if (!grid || (!grid->enabled && !grid->project_path) || !playlist)
        return false;
    mp_mutex_lock(&grid->lock);
    struct grid_tile *tile = &grid->tiles[0];
    if (!tile->num_items) {
        mp_mutex_unlock(&grid->lock);
        return false;
    }
    playlist_clear(playlist);
    for (int i = 0; i < tile->num_items; i++)
        playlist_append_file(playlist, tile->items[i].path);
    playlist->current = playlist_entry_from_index(playlist, tile->current_index);
    mp_mutex_unlock(&grid->lock);
    return playlist->current != NULL;
}

static void attach_shared_device(struct mp_grid *grid, struct vo *vo)
{
    av_buffer_unref(&grid->shared_hw_device);
    if (!vo || !vo->hwdec_devs)
        return;
    struct hwdec_imgfmt_request request = {
        .imgfmt = IMGFMT_D3D11,
        .probing = true,
    };
    hwdec_devices_request_for_img_fmt(vo->hwdec_devs, &request);
    struct mp_hwdec_ctx *ctx = hwdec_devices_get_by_imgfmt_and_type(
        vo->hwdec_devs, IMGFMT_D3D11, AV_HWDEVICE_TYPE_D3D11VA);
    if (ctx && ctx->av_device_ref)
        grid->shared_hw_device = av_buffer_ref(ctx->av_device_ref);
}

double mp_grid_file_loaded(struct mp_grid *grid, const char *filename,
                           double duration, struct vo *vo)
{
    if (!grid || !filename || duration <= 0)
        return MP_NOPTS_VALUE;
    mp_grid_set_vo(grid, vo);
    int aspect_w = 16, aspect_h = 9;
    if (vo) {
        mp_mutex_lock(&vo->params_mutex);
        if (vo->params)
            mp_image_params_get_dsize(vo->params, &aspect_w, &aspect_h);
        mp_mutex_unlock(&vo->params_mutex);
    }
    if (aspect_w <= 0 || aspect_h <= 0) {
        aspect_w = 16;
        aspect_h = 9;
    }
    mp_mutex_lock(&grid->lock);
    grid->aspect_w = aspect_w;
    grid->aspect_h = aspect_h;
    struct grid_item *main_item = current_item(&grid->tiles[0]);
    if (main_item)
        main_item->duration = duration;
    mp_mutex_unlock(&grid->lock);

    bool session_enabled = false;
    int session_rows = 0, session_columns = 0, num_positions = 0;
    double positions[MP_GRID_MAX_TILES] = {0};
    bool have_session = false;
    if (grid->automatic_mode && !grid->session_restore_checked) {
        have_session = load_last_session(grid, filename, &session_enabled,
                                         &session_rows, &session_columns,
                                         positions, &num_positions);
        grid->session_restore_checked = true;
        grid->last_session_save_ns = mp_time_ns();
    }
    grid->session_invalidated = false;

    if (!grid->enabled) {
        if (grid->project_path) {
            mp_mutex_lock(&grid->lock);
            struct grid_item *item = current_item(&grid->tiles[0]);
            double position = item
                ? tile_clock_locked(&grid->tiles[0], mp_time_ns())
                : MP_NOPTS_VALUE;
            mp_mutex_unlock(&grid->lock);
            return position == MP_NOPTS_VALUE
                    ? position : MPCLAMP(position, 0, duration);
        }
        if (have_session && !session_enabled && num_positions > 0)
            return positions[0];
        return MP_NOPTS_VALUE;
    }

    bool resume_layout = have_session && session_enabled &&
                         session_rows == grid->rows &&
                         session_columns == grid->columns &&
                         num_positions >= layout_count(grid);
    attach_shared_device(grid, vo);
    int count = layout_count(grid);
    mp_mutex_lock(&grid->lock);
    audio_fifo_reconcile_locked(grid, count);
    for (int i = 0; i < count; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        tile->hidden = false;
        if (!tile->num_items) {
            MP_TARRAY_GROW(grid, tile->items, tile->num_items);
            struct grid_item *item = &tile->items[tile->num_items++];
            item->path = talloc_strdup(tile->items, filename);
            item->absolute_hint = mp_normalize_path(tile->items, filename);
            item->fixed_start = resume_layout
                                    ? MPCLAMP(positions[i], 0, duration)
                                    : duration * (i + 0.5) / count;
            item->resume_position = item->fixed_start;
            item->duration = duration;
            item->opened_resume_position = item->resume_position;
            tile->current_index = 0;
            tile->fixed_start = item->fixed_start;
            set_tile_clock_locked(tile, item->fixed_start, mp_time_ns());
        }
        if (i > 0)
            start_decoder_locked(grid, i);
    }
    for (int i = count; i < MP_GRID_MAX_TILES; i++)
        grid->tiles[i].hidden = true;
    bool uniform_source = count > 0;
    for (int i = 0; i < count; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        if (tile->num_items != 1 || !tile->items[0].path ||
            strcmp(tile->items[0].path, filename) != 0)
        {
            uniform_source = false;
            break;
        }
    }
    grid->single_source_auto = uniform_source;
    grid->single_source_duration = uniform_source ? duration : 0;
    double start = grid->tiles[0].num_items ?
        tile_clock_locked(&grid->tiles[0], mp_time_ns()) : duration * 0.5 / count;
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);
    MP_INFO(grid, "Grid playback initialized: %dx%d, shared D3D11 device: %s\n",
            grid->rows, grid->columns, grid->shared_hw_device ? "yes" : "no");
    return start;
}

void mp_grid_file_started(struct mp_grid *grid)
{
    if (!grid)
        return;
    mp_mutex_lock(&grid->lock);
    grid->media_active = true;
    mp_mutex_unlock(&grid->lock);
}

void mp_grid_file_unloading(struct mp_grid *grid, bool eof)
{
    if (!grid)
        return;
    save_last_session(grid, eof);
    mp_mutex_lock(&grid->lock);
    grid->media_active = false;
    mp_mutex_unlock(&grid->lock);
    if (!grid->enabled)
        return;
    if (eof) {
        mp_mutex_lock(&grid->lock);
        struct grid_tile *tile = &grid->tiles[0];
        if (tile->num_items > 1)
            tile->current_index = (tile->current_index + 1) % tile->num_items;
        struct grid_item *item = current_item(tile);
        if (item)
            set_tile_clock_locked(tile, item->fixed_start, mp_time_ns());
        mp_mutex_unlock(&grid->lock);
    }
    mp_grid_update_resume(grid);
    if (grid->project_path)
        mp_grid_save_project(grid, NULL, false);
}

bool mp_grid_enabled(struct mp_grid *grid)
{
    return grid && grid->enabled;
}

bool mp_grid_empty(struct mp_grid *grid)
{
    if (!grid || !grid->enabled)
        return false;
    mp_mutex_lock(&grid->lock);
    bool empty = true;
    for (int i = 0; i < MP_GRID_MAX_TILES; i++) {
        if (grid->tiles[i].num_items) {
            empty = false;
            break;
        }
    }
    mp_mutex_unlock(&grid->lock);
    return empty;
}

bool mp_grid_snapshot(struct mp_grid *grid, struct mp_grid_snapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    if (!grid || !grid->enabled)
        return false;
    mp_mutex_lock(&grid->lock);
    snapshot->enabled = true;
    snapshot->rows = grid->rows;
    snapshot->columns = grid->columns;
    snapshot->aspect_w = grid->aspect_w;
    snapshot->aspect_h = grid->aspect_h;
    snapshot->num_cells = layout_count(grid);
    snapshot->active_tile = grid->active_tile;
    int64_t now = mp_time_ns();
    for (int i = 0; i < snapshot->num_cells; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        struct mp_grid_snapshot_cell *cell = &snapshot->cells[i];
        cell->image = mp_image_new_ref(tile->frame);
        cell->tile_id = i;
        cell->selected = i == grid->active_tile;
        cell->paused = tile->paused;
        cell->muted = tile->muted;
        cell->position = tile_clock_locked(tile, now);
        cell->volume = tile->volume;
        cell->speed = tile->speed;
        cell->zoom = tile->zoom_active ? 2.0 : 1.0;
        cell->center_x = tile->center_x;
        cell->center_y = tile->center_y;
    }
    mp_mutex_unlock(&grid->lock);
    return true;
}

void mp_grid_snapshot_free(struct mp_grid_snapshot *snapshot)
{
    for (int i = 0; i < snapshot->num_cells; i++)
        talloc_free(snapshot->cells[i].image);
    memset(snapshot, 0, sizeof(*snapshot));
}

bool mp_grid_vo_snapshot(void *ctx, struct mp_grid_snapshot *snapshot)
{
    struct mp_grid *grid = ctx;
    // The first frame can reach the VO after file-loaded, so vo->params is the
    // authoritative point at which the display aspect is guaranteed to exist.
    if (grid && grid->vo && grid->vo->params) {
        int aspect_w = 0, aspect_h = 0;
        mp_image_params_get_dsize(grid->vo->params, &aspect_w, &aspect_h);
        if (aspect_w > 0 && aspect_h > 0) {
            mp_mutex_lock(&grid->lock);
            grid->aspect_w = aspect_w;
            grid->aspect_h = aspect_h;
            mp_mutex_unlock(&grid->lock);
        }
    }
    return mp_grid_snapshot(grid, snapshot);
}

bool mp_grid_vo_empty(void *ctx)
{
    return mp_grid_empty(ctx);
}

bool mp_grid_vo_layout(void *ctx, int *rows, int *columns)
{
    struct mp_grid *grid = ctx;
    if (!grid || !grid->enabled || !rows || !columns)
        return false;
    mp_mutex_lock(&grid->lock);
    *rows = grid->rows;
    *columns = grid->columns;
    bool valid = grid->media_active && valid_layout(*rows, *columns);
    mp_mutex_unlock(&grid->lock);
    return valid;
}

void mp_grid_request_redraw(struct mp_grid *grid)
{
    if (!grid)
        return;
    mp_mutex_lock(&grid->lock);
    if (grid->vo)
        vo_redraw(grid->vo);
    mp_mutex_unlock(&grid->lock);
}

void mp_grid_set_vo(struct mp_grid *grid, struct vo *vo)
{
    if (!grid)
        return;
    mp_mutex_lock(&grid->lock);
    grid->vo = vo;
    mp_mutex_unlock(&grid->lock);
}

static double read_pcm_sample(void **data, int format, int num_channels,
                              int sample, int channel)
{
    bool planar = af_fmt_is_planar(format);
    int base = af_fmt_from_planar(format);
    int index = planar ? sample : sample * num_channels + channel;
    void *plane = data[planar ? channel : 0];
    switch (base) {
    case AF_FORMAT_U8:
        return (((uint8_t *)plane)[index] - 128.0) / 128.0;
    case AF_FORMAT_S16:
        return ((int16_t *)plane)[index] / 32768.0;
    case AF_FORMAT_S32:
        return ((int32_t *)plane)[index] / 2147483648.0;
    case AF_FORMAT_S64:
        return ((int64_t *)plane)[index] / 9223372036854775808.0;
    case AF_FORMAT_FLOAT:
        return ((float *)plane)[index];
    case AF_FORMAT_DOUBLE:
        return ((double *)plane)[index];
    default:
        return 0;
    }
}

static void write_pcm_sample(void **data, int format, int num_channels,
                             int sample, int channel, double value)
{
    bool planar = af_fmt_is_planar(format);
    int base = af_fmt_from_planar(format);
    int index = planar ? sample : sample * num_channels + channel;
    void *plane = data[planar ? channel : 0];
    value = MPCLAMP(value, -1.0, 1.0);
    switch (base) {
    case AF_FORMAT_U8:
        ((uint8_t *)plane)[index] = MPCLAMP(lrint(value * 127.0 + 128.0),
                                            0, UINT8_MAX);
        break;
    case AF_FORMAT_S16:
        ((int16_t *)plane)[index] = value <= -1.0 ? INT16_MIN
                                      : lrint(value * INT16_MAX);
        break;
    case AF_FORMAT_S32:
        ((int32_t *)plane)[index] = value <= -1.0 ? INT32_MIN
                                      : llrint(value * INT32_MAX);
        break;
    case AF_FORMAT_S64:
        ((int64_t *)plane)[index] = value <= -1.0 ? INT64_MIN
                                      : value >= 1.0 ? INT64_MAX
                                      : llrint(value * (double)INT64_MAX);
        break;
    case AF_FORMAT_FLOAT:
        ((float *)plane)[index] = value;
        break;
    case AF_FORMAT_DOUBLE:
        ((double *)plane)[index] = value;
        break;
    }
}

void mp_grid_mix_audio(void *ctx, void **data, int samples, int rate,
                       int format, const struct mp_chmap *channels)
{
    struct mp_grid *grid = ctx;
    if (!grid || !grid->enabled || !data || samples <= 0 || rate <= 0 ||
        !af_fmt_is_pcm(format) || af_fmt_is_spdif(format) ||
        !channels || channels->num < 1)
        return;

    mp_mutex_lock(&grid->lock);
    int count = layout_count(grid);
    int active = 0;
    for (int i = 0; i < count; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        bool selected = grid->solo_active || audio_fifo_contains_locked(grid, i);
        active += selected && !tile->hidden && !tile->paused && !tile->muted &&
                  tile->volume > 0 && tile->num_items > 0;
    }
    float norm = active > 1 ? 1.0f / sqrtf(active) : 1.0f;
    struct grid_tile *main = &grid->tiles[0];

    int64_t now = mp_time_ns();
    for (int i = 1; i < count; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        struct grid_decoder *decoder = &tile->decoder;
        if (tile->paused || tile->hidden || decoder->audio_count <= 0 ||
            decoder->audio_output_rate <= 0)
            continue;
        double lag = tile_clock_locked(tile, now) -
                     decoder->audio_read_position;
        if (lag <= GRID_AUDIO_MAX_LAG)
            continue;
        int drop = MPCLAMP((int)floor((lag - GRID_AUDIO_TARGET_LAG) *
                                      decoder->audio_output_rate),
                           0, decoder->audio_count);
        decoder->audio_read = (decoder->audio_read + drop) %
                              decoder->audio_capacity;
        decoder->audio_count -= drop;
        decoder->audio_read_position +=
            (double)drop / decoder->audio_output_rate;
        decoder->audio_sync_dropped += drop;
        if (!decoder->audio_count)
            decoder->audio_resync = true;
    }

    for (int s = 0; s < samples; s++) {
        float mix_l = 0, mix_r = 0;
        bool main_selected = grid->solo_active ||
                             audio_fifo_contains_locked(grid, 0);
        if (main_selected && !main->paused && !main->hidden && !main->muted &&
            main->volume > 0 && main->num_items > 0)
        {
            mix_l = read_pcm_sample(data, format, channels->num, s, 0) *
                    main->volume;
            mix_r = channels->num > 1
                        ? read_pcm_sample(data, format, channels->num, s, 1) *
                          main->volume
                        : mix_l;
            main->audio_mixed_samples++;
        }

        for (int i = 1; i < count; i++) {
            struct grid_tile *tile = &grid->tiles[i];
            struct grid_decoder *decoder = &tile->decoder;
            float l = 0, r = 0;
            if (!tile->paused && !tile->hidden && decoder->audio_count > 0) {
                int at = decoder->audio_read;
                l = decoder->audio_ring[at * 2 + 0];
                r = decoder->audio_ring[at * 2 + 1];
                decoder->audio_resample_phase += 48000.0 / rate;
                int advance = decoder->audio_resample_phase;
                decoder->audio_resample_phase -= advance;
                advance = MPMIN(advance, decoder->audio_count);
                decoder->audio_read = (at + advance) % decoder->audio_capacity;
                decoder->audio_count -= advance;
                decoder->audio_read_position +=
                    (double)advance / decoder->audio_output_rate;
                if (!decoder->audio_count)
                    decoder->audio_resync = true;
            }
            bool selected = grid->solo_active ||
                            audio_fifo_contains_locked(grid, i);
            if (selected && !tile->muted) {
                mix_l += l * tile->volume;
                mix_r += r * tile->volume;
                if (l != 0 || r != 0)
                    tile->audio_mixed_samples++;
            }
        }

        mix_l *= norm;
        mix_r *= norm;
        // Smooth asymptotic limiter. Master volume is applied afterwards by AO.
        mix_l = mix_l / (1.0f + 0.12f * fabsf(mix_l));
        mix_r = mix_r / (1.0f + 0.12f * fabsf(mix_r));
        if (channels->num == 1) {
            write_pcm_sample(data, format, channels->num, s, 0,
                             (mix_l + mix_r) * 0.5);
        } else {
            write_pcm_sample(data, format, channels->num, s, 0, mix_l);
            write_pcm_sample(data, format, channels->num, s, 1, mix_r);
            // Do not leave an unmodified copy of the main tile in surround
            // channels: every physical output channel belongs to the mixer.
            for (int c = 2; c < channels->num; c++)
                write_pcm_sample(data, format, channels->num, s, c, 0);
        }
    }
    mp_mutex_unlock(&grid->lock);
}

static void reconfigure_grid_window(struct mp_grid *grid, struct vo *vo)
{
    if (!vo)
        return;
    struct mp_image_params params;
    bool have_params = false;
    mp_mutex_lock(&vo->params_mutex);
    if (vo->params) {
        params = *vo->params;
        have_params = true;
    }
    mp_mutex_unlock(&vo->params_mutex);
    if (have_params)
        vo_reconfig(vo, &params);
}

bool mp_grid_disable(struct mp_grid *grid)
{
    if (!grid)
        return false;
    mp_mutex_lock(&grid->lock);
    bool was_enabled = grid->enabled;
    grid->enabled = false;
    for (int i = 1; i < MP_GRID_MAX_TILES; i++) {
        grid->tiles[i].hidden = true;
        grid->tiles[i].decoder.command_serial++;
    }
    mp_cond_broadcast(&grid->wakeup);
    struct vo *vo = grid->vo;
    mp_mutex_unlock(&grid->lock);

    restore_normal_looping(grid);
    save_last_layout(grid);
    save_last_session(grid, false);
    mp_input_disable_section(grid->mpctx->input, "grid");
    if (was_enabled)
        reconfigure_grid_window(grid, vo);
    mp_grid_request_redraw(grid);
    MP_INFO(grid, "Grid switched to 1x1 normal playback mode.\n");
    return true;
}

bool mp_grid_set_layout(struct mp_grid *grid, int rows, int columns)
{
    if (!grid || !valid_layout(rows, columns))
        return false;
    struct MPContext *mpctx = grid->mpctx;
    bool playback_single = mpctx->playback_initialized && mpctx->filename &&
                           mpctx->playlist &&
                           mpctx->playlist->num_entries == 1;
    double playback_duration = playback_single ? get_time_length(mpctx)
                                                : MP_NOPTS_VALUE;
    char *playback_path = playback_single
                            ? talloc_strdup(NULL, mpctx->filename) : NULL;
    mp_mutex_lock(&grid->lock);
    bool was_enabled = grid->enabled;
    grid->enabled = true;
    int old_rows = grid->rows;
    int old_columns = grid->columns;
    int old = layout_count(grid);
    grid->rows = rows;
    grid->columns = columns;
    int count = layout_count(grid);
    audio_fifo_reconcile_locked(grid, count);
    bool layout_changed = old_rows != rows || old_columns != columns;
    bool reenter_single = !was_enabled && playback_single &&
                          playback_duration > 0 && !grid->project_path &&
                          (grid->single_source_auto ||
                           grid->tiles[0].num_items == 0);
    bool restart_single = reenter_single ||
                          (layout_changed && grid->single_source_auto &&
                           grid->single_source_duration > 0 &&
                           grid->tiles[0].num_items == 1);
    char *single_path = restart_single
                            ? talloc_strdup(NULL, reenter_single
                                ? playback_path : grid->tiles[0].items[0].path)
                            : NULL;
    double single_duration = reenter_single ? playback_duration
                                             : grid->single_source_duration;
    if (reenter_single) {
        grid->single_source_auto = true;
        grid->single_source_duration = playback_duration;
    }
    for (int i = 0; i < MP_GRID_MAX_TILES; i++) {
        bool hidden = i >= count;
        struct grid_tile *tile = &grid->tiles[i];
        if (restart_single && !hidden) {
            talloc_free(tile->items);
            tile->items = NULL;
            tile->num_items = 0;
            MP_TARRAY_GROW(grid, tile->items, tile->num_items);
            struct grid_item *item = &tile->items[tile->num_items++];
            double start = single_duration * i / count;
            *item = (struct grid_item) {
                .path = talloc_strdup(tile->items, single_path),
                .absolute_hint = mp_normalize_path(tile->items, single_path),
                .fixed_start = start,
                .resume_position = start,
                .duration = single_duration,
                .opened_resume_position = start,
            };
            tile->current_index = 0;
            tile->paused = false;
            tile->fixed_start = start;
            set_tile_clock_locked(tile, start, mp_time_ns());
            tile->decoder.seek_target = start;
            tile->decoder.seek_pending = true;
            tile->decoder.reload = true;
            tile->decoder.command_serial++;
            set_error(tile, NULL);
        }
        if (tile->hidden != hidden) {
            tile->hidden = hidden;
            tile->decoder.command_serial++;
        }
        if (i > 0 && i < count && tile->num_items)
            start_decoder_locked(grid, i);
    }
    if (grid->active_tile >= count)
        grid->active_tile = count - 1;
    mp_cond_broadcast(&grid->wakeup);
    struct vo *vo = grid->vo;
    mp_mutex_unlock(&grid->lock);
    talloc_free(single_path);
    talloc_free(playback_path);
    if (!was_enabled) {
        apply_grid_looping(grid);
        mp_input_enable_section(grid->mpctx->input, "grid", MP_INPUT_ON_TOP);
        if (!restart_single && grid->mpctx->playback_initialized &&
            grid->mpctx->filename)
        {
            double current = get_current_time(grid->mpctx);
            double duration = get_time_length(grid->mpctx);
            mp_grid_file_loaded(grid, grid->mpctx->filename, duration, vo);
            if (current != MP_NOPTS_VALUE) {
                mp_mutex_lock(&grid->lock);
                set_tile_clock_locked(&grid->tiles[0], current, mp_time_ns());
                struct grid_item *item = current_item(&grid->tiles[0]);
                if (item)
                    item->resume_position = current;
                mp_mutex_unlock(&grid->lock);
            }
        }
    }
    if (restart_single && grid->mpctx->playback_initialized) {
        set_pause_state(grid->mpctx, false);
        queue_seek(grid->mpctx, MPSEEK_ABSOLUTE, 0, MPSEEK_DEFAULT, 0);
    }
    MP_VERBOSE(grid, "Grid layout changed from %d cells to %dx%d.\n", old, rows, columns);
    save_last_layout(grid);
    if (layout_changed || !was_enabled)
        reconfigure_grid_window(grid, vo);
    mp_grid_request_redraw(grid);
    return true;
}

bool mp_grid_set_active(struct mp_grid *grid, int tile)
{
    if (!grid || tile < 0 || tile >= layout_count(grid))
        return false;
    mp_mutex_lock(&grid->lock);
    grid->active_tile = tile;
    audio_fifo_touch_locked(grid, tile);
    mp_mutex_unlock(&grid->lock);
    mp_grid_request_redraw(grid);
    return true;
}

int mp_grid_get_active(struct mp_grid *grid)
{
    if (!grid || !grid->enabled)
        return -1;
    mp_mutex_lock(&grid->lock);
    int active = grid->active_tile;
    mp_mutex_unlock(&grid->lock);
    return active;
}

bool mp_grid_cycle_active(struct mp_grid *grid, int direction)
{
    if (!grid || !grid->enabled || !direction)
        return false;
    mp_mutex_lock(&grid->lock);
    int count = layout_count(grid);
    grid->active_tile = (grid->active_tile + (direction > 0 ? 1 : count - 1)) % count;
    mp_mutex_unlock(&grid->lock);
    mp_grid_request_redraw(grid);
    return true;
}

bool mp_grid_append(struct mp_grid *grid, int tile_index, const char *path,
                    double fixed_start)
{
    if (grid && tile_index == -1)
        tile_index = grid->active_tile;
    if (!grid || !path || tile_index < 0 || tile_index >= MP_GRID_MAX_TILES)
        return false;
    struct grid_tile *tile = &grid->tiles[tile_index];
    mp_mutex_lock(&grid->lock);
    grid->single_source_auto = false;
    grid->single_source_duration = 0;
    MP_TARRAY_GROW(grid, tile->items, tile->num_items);
    struct grid_item *item = &tile->items[tile->num_items++];
    *item = (struct grid_item) {
        .path = talloc_strdup(tile->items, path),
        .absolute_hint = mp_normalize_path(tile->items, path),
        .fixed_start = MPMAX(0, fixed_start),
        .resume_position = MPMAX(0, fixed_start),
        .opened_resume_position = MPMAX(0, fixed_start),
    };
    if (tile->current_index < 0) {
        tile->current_index = 0;
        set_tile_clock_locked(tile, item->fixed_start, mp_time_ns());
        tile->decoder.command_serial++;
        tile->decoder.reload = true;
    }
    if (tile_index > 0)
        start_decoder_locked(grid, tile_index);
    bool append_main = tile_index == 0 && grid->mpctx->playback_initialized;
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);
    if (append_main)
        playlist_append_file(grid->mpctx->playlist, path);
    return true;
}

bool mp_grid_drop_files(struct mp_grid *grid, int tile_index, int num_files,
                        const char **paths)
{
    if (!grid || !grid->enabled || tile_index < 0 ||
        tile_index >= layout_count(grid) || num_files <= 0 || !paths)
        return false;

    // The first file dropped into an empty Grid is equivalent to opening that
    // file. Let the normal file-loaded hook discover its duration and fill all
    // visible tiles with evenly distributed starting positions. This must not
    // depend on which tile happened to receive the drop.
    if (num_files == 1 && paths[0] && paths[0][0] && mp_grid_empty(grid)) {
        struct MPContext *mpctx = grid->mpctx;
        mp_mutex_lock(&grid->lock);
        grid->active_tile = tile_index;
        mp_mutex_unlock(&grid->lock);
        playlist_clear(mpctx->playlist);
        playlist_append_file(mpctx->playlist, paths[0]);
        mpctx->playlist->current = playlist_get_first(mpctx->playlist);
        mp_set_playlist_entry(mpctx, mpctx->playlist->current);
        mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
        return true;
    }

    struct grid_tile *tile = &grid->tiles[tile_index];
    mp_mutex_lock(&grid->lock);
    int first_new = tile->num_items;
    for (int i = 0; i < num_files; i++) {
        if (!paths[i] || !paths[i][0])
            continue;
        MP_TARRAY_GROW(grid, tile->items, tile->num_items);
        struct grid_item *item = &tile->items[tile->num_items++];
        *item = (struct grid_item) {
            .path = talloc_strdup(tile->items, paths[i]),
            .absolute_hint = mp_normalize_path(tile->items, paths[i]),
            .fixed_start = 0,
            .resume_position = 0,
            .opened_resume_position = 0,
        };
    }
    if (tile->num_items == first_new) {
        mp_mutex_unlock(&grid->lock);
        return false;
    }
    grid->single_source_auto = false;
    grid->single_source_duration = 0;

    // A drop is an explicit request to see the dropped media now. Preserve
    // the existing queue, but select the first newly appended entry.
    tile->current_index = first_new;
    set_tile_clock_locked(tile, 0, mp_time_ns());
    tile->decoder.seek_pending = false;
    tile->decoder.command_serial++;
    tile->decoder.reload = true;
    if (tile_index > 0)
        start_decoder_locked(grid, tile_index);
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);

    if (tile_index == 0) {
        struct MPContext *mpctx = grid->mpctx;
        int first_playlist = mpctx->playlist->num_entries;
        for (int i = 0; i < num_files; i++) {
            if (paths[i] && paths[i][0])
                playlist_append_file(mpctx->playlist, paths[i]);
        }
        struct playlist_entry *entry =
            playlist_entry_from_index(mpctx->playlist, first_playlist);
        if (entry)
            mp_set_playlist_entry(mpctx, entry);
        mp_notify(mpctx, MP_EVENT_CHANGE_PLAYLIST, NULL);
    }
    mp_grid_request_redraw(grid);
    return true;
}

bool mp_grid_remove(struct mp_grid *grid, int tile_index, int index)
{
    if (grid && tile_index == -1)
        tile_index = grid->active_tile;
    if (!grid || tile_index < 0 || tile_index >= MP_GRID_MAX_TILES)
        return false;
    struct grid_tile *tile = &grid->tiles[tile_index];
    mp_mutex_lock(&grid->lock);
    if (index < 0)
        index = tile->current_index;
    if (index < 0 || index >= tile->num_items) {
        mp_mutex_unlock(&grid->lock);
        return false;
    }
    grid->single_source_auto = false;
    grid->single_source_duration = 0;
    talloc_free(tile->items[index].path);
    talloc_free(tile->items[index].absolute_hint);
    MP_TARRAY_REMOVE_AT(tile->items, tile->num_items, index);
    if (!tile->num_items)
        tile->current_index = -1;
    else if (tile->current_index >= tile->num_items)
        tile->current_index = 0;
    tile->decoder.command_serial++;
    tile->decoder.reload = true;
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);
    return true;
}

bool mp_grid_seek(struct mp_grid *grid, int tile_index, double value, bool absolute)
{
    if (grid && tile_index == -1)
        tile_index = grid->active_tile;
    if (!grid || tile_index < 0 || tile_index >= layout_count(grid))
        return false;
    struct grid_tile *tile = &grid->tiles[tile_index];
    mp_mutex_lock(&grid->lock);
    double pos = absolute ? value : tile_clock_locked(tile, mp_time_ns()) + value;
    pos = MPMAX(0, pos);
    set_tile_clock_locked(tile, pos, mp_time_ns());
    tile->decoder.seek_target = pos;
    tile->decoder.seek_pending = true;
    tile->decoder.command_serial++;
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);
    return true;
}

bool mp_grid_get_progress(struct mp_grid *grid, int tile_index,
                          double *position, double *duration,
                          int *rows, int *columns)
{
    if (!grid || tile_index < 0 || tile_index >= layout_count(grid))
        return false;
    mp_mutex_lock(&grid->lock);
    struct grid_tile *tile = &grid->tiles[tile_index];
    struct grid_item *item = current_item(tile);
    bool ok = item != NULL;
    if (ok) {
        if (position)
            *position = tile_clock_locked(tile, mp_time_ns());
        if (duration)
            *duration = item->duration;
        if (rows)
            *rows = grid->rows;
        if (columns)
            *columns = grid->columns;
    }
    mp_mutex_unlock(&grid->lock);
    return ok;
}

bool mp_grid_set_paused(struct mp_grid *grid, int tile_index, bool paused)
{
    if (grid && tile_index == -1)
        tile_index = grid->active_tile;
    if (!grid || tile_index < 0 || tile_index >= layout_count(grid))
        return false;
    mp_mutex_lock(&grid->lock);
    struct grid_tile *tile = &grid->tiles[tile_index];
    double pos = tile_clock_locked(tile, mp_time_ns());
    tile->paused = paused;
    set_tile_clock_locked(tile, pos, mp_time_ns());
    tile->decoder.command_serial++;
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);
    return true;
}

bool mp_grid_set_mute(struct mp_grid *grid, int tile, bool muted)
{
    if (grid && tile == -1)
        tile = grid->active_tile;
    if (!grid || tile < 0 || tile >= layout_count(grid)) return false;
    mp_mutex_lock(&grid->lock); grid->tiles[tile].muted = muted; mp_mutex_unlock(&grid->lock);
    return true;
}

bool mp_grid_toggle_solo(struct mp_grid *grid, int tile)
{
    if (grid && tile == -1)
        tile = grid->active_tile;
    if (!grid || tile < 0 || tile >= layout_count(grid))
        return false;
    mp_mutex_lock(&grid->lock);
    int count = layout_count(grid);
    if (grid->solo_active) {
        for (int i = 0; i < count; i++)
            grid->tiles[i].muted = grid->solo_saved[i];
        grid->solo_active = false;
    } else {
        for (int i = 0; i < count; i++) {
            grid->solo_saved[i] = grid->tiles[i].muted;
            grid->tiles[i].muted = i != tile;
        }
        grid->solo_active = true;
    }
    mp_mutex_unlock(&grid->lock);
    return true;
}

bool mp_grid_set_volume(struct mp_grid *grid, int tile, double volume)
{
    if (grid && tile == -1)
        tile = grid->active_tile;
    if (!grid || tile < 0 || tile >= layout_count(grid) || volume < 0 || volume > 2) return false;
    mp_mutex_lock(&grid->lock); grid->tiles[tile].volume = volume; mp_mutex_unlock(&grid->lock);
    return true;
}

bool mp_grid_set_speed(struct mp_grid *grid, int tile_index, double speed)
{
    if (grid && tile_index == -1)
        tile_index = grid->active_tile;
    if (!grid || tile_index < 0 || tile_index >= layout_count(grid) || speed < 0.1 || speed > 8) return false;
    mp_mutex_lock(&grid->lock);
    struct grid_tile *tile = &grid->tiles[tile_index];
    double pos = tile_clock_locked(tile, mp_time_ns());
    tile->speed = speed;
    set_tile_clock_locked(tile, pos, mp_time_ns());
    tile->decoder.command_serial++;
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);
    return true;
}

bool mp_grid_set_fixed_start(struct mp_grid *grid, int tile_index, double position)
{
    if (grid && tile_index == -1)
        tile_index = grid->active_tile;
    if (!grid || tile_index < 0 || tile_index >= layout_count(grid)) return false;
    mp_mutex_lock(&grid->lock);
    struct grid_tile *tile = &grid->tiles[tile_index];
    struct grid_item *item = current_item(tile);
    if (isnan(position))
        position = tile_clock_locked(tile, mp_time_ns());
    if (item) item->fixed_start = MPMAX(0, position);
    tile->fixed_start = MPMAX(0, position);
    mp_mutex_unlock(&grid->lock);
    return item != NULL;
}

bool mp_grid_set_zoom(struct mp_grid *grid, int tile_index, bool active,
                      double center_x, double center_y)
{
    if (grid && tile_index == -1)
        tile_index = grid->active_tile;
    if (!grid || tile_index < 0 || tile_index >= layout_count(grid)) return false;
    mp_mutex_lock(&grid->lock);
    struct grid_tile *tile = &grid->tiles[tile_index];
    tile->zoom_active = active;
    tile->center_x = MPCLAMP(center_x, 0, 1);
    tile->center_y = MPCLAMP(center_y, 0, 1);
    mp_mutex_unlock(&grid->lock);
    mp_grid_request_redraw(grid);
    return true;
}

static struct mp_rect grid_calc_output_rect(int width, int height, int rows,
                                            int columns, int aspect_w,
                                            int aspect_h, bool allow_upscale)
{
    struct mp_rect rect = {0, 0, width, height};
    if (width <= 0 || height <= 0 || rows <= 0 || columns <= 0 ||
        width < columns || height < rows ||
        aspect_w <= 0 || aspect_h <= 0)
        return rect;

    int64_t native_w = (int64_t)aspect_w * columns;
    int64_t native_h = (int64_t)aspect_h * rows;
    double scale = MPMIN(width / (double)native_w,
                         height / (double)native_h);
    if (!allow_upscale)
        scale = MPMIN(scale, 1.0);
    int canvas_w = MPCLAMP(llrint(native_w * scale), 1, width);
    int canvas_h = MPCLAMP(llrint(native_h * scale), 1, height);
    rect.x0 = (width - canvas_w) / 2;
    rect.y0 = (height - canvas_h) / 2;
    rect.x1 = rect.x0 + canvas_w;
    rect.y1 = rect.y0 + canvas_h;
    return rect;
}

struct mp_rect mp_grid_calc_output_rect(int width, int height, int rows,
                                        int columns, int aspect_w,
                                        int aspect_h)
{
    // The first/main video's display resolution defines the maximum tile
    // size. A larger window or fullscreen surface adds symmetric outer
    // padding instead of upscaling the videos; a smaller surface uniformly
    // shrinks the complete, tightly packed Grid canvas.
    return grid_calc_output_rect(width, height, rows, columns, aspect_w,
                                 aspect_h, false);
}

struct mp_rect mp_grid_output_rect(struct mp_grid *grid, int width, int height)
{
    if (!grid)
        return (struct mp_rect){0, 0, width, height};
    return mp_grid_calc_output_rect(width, height, grid->rows, grid->columns,
                                    grid->aspect_w, grid->aspect_h);
}

int mp_grid_hit_test(struct mp_grid *grid, int x, int y, int width, int height)
{
    if (!grid || !grid->enabled || width <= 0 || height <= 0 ||
        x < 0 || y < 0 || x >= width || y >= height)
        return -1;
    struct mp_rect canvas = mp_grid_output_rect(grid, width, height);
    if (!mp_rect_contains(&canvas, x, y))
        return -1;
    int col = MPMIN(grid->columns - 1,
                    (x - canvas.x0) * grid->columns / mp_rect_w(canvas));
    int row = MPMIN(grid->rows - 1,
                    (y - canvas.y0) * grid->rows / mp_rect_h(canvas));
    return row * grid->columns + col;
}

bool mp_grid_zoom_at(struct mp_grid *grid, int x, int y, int width, int height,
                     bool active, bool move_only)
{
    if (!grid || !grid->enabled || width <= 0 || height <= 0)
        return false;
    mp_mutex_lock(&grid->lock);
    int tile_index = grid->active_tile;
    if (!move_only) {
        int hit = mp_grid_hit_test(grid, x, y, width, height);
        if (hit >= 0) {
            tile_index = grid->active_tile = hit;
            audio_fifo_touch_locked(grid, hit);
        }
    }
    struct grid_tile *tile = &grid->tiles[tile_index];
    if (move_only && !tile->zoom_active) {
        mp_mutex_unlock(&grid->lock);
        return false;
    }
    int col = tile_index % grid->columns;
    int row = tile_index / grid->columns;
    struct mp_rect canvas = mp_grid_output_rect(grid, width, height);
    int x0 = canvas.x0 + mp_rect_w(canvas) * col / grid->columns;
    int x1 = canvas.x0 + mp_rect_w(canvas) * (col + 1) / grid->columns;
    int y0 = canvas.y0 + mp_rect_h(canvas) * row / grid->rows;
    int y1 = canvas.y0 + mp_rect_h(canvas) * (row + 1) / grid->rows;
    tile->center_x = MPCLAMP((double)(x - x0) / MPMAX(1, x1 - x0), 0, 1);
    tile->center_y = MPCLAMP((double)(y - y0) / MPMAX(1, y1 - y0), 0, 1);
    if (!move_only)
        tile->zoom_active = active;
    mp_mutex_unlock(&grid->lock);
    mp_grid_request_redraw(grid);
    return true;
}

static bool node_number(struct mpv_node *map, const char *key, double *out)
{
    struct mpv_node *n = node_map_get(map, key);
    if (!n) return false;
    if (n->format == MPV_FORMAT_DOUBLE) *out = n->u.double_;
    else if (n->format == MPV_FORMAT_INT64) *out = n->u.int64;
    else return false;
    return true;
}

static bool node_int(struct mpv_node *map, const char *key, int *out)
{
    struct mpv_node *n = node_map_get(map, key);
    if (!n || n->format != MPV_FORMAT_INT64 || n->u.int64 < INT_MIN || n->u.int64 > INT_MAX)
        return false;
    *out = n->u.int64;
    return true;
}

static const char *node_string(struct mpv_node *map, const char *key)
{
    struct mpv_node *n = node_map_get(map, key);
    return n && n->format == MPV_FORMAT_STRING ? n->u.string : NULL;
}

static bool paths_equal(const char *a, const char *b)
{
    if (!a || !b)
        return false;
#if HAVE_DOS_PATHS
    return !strcasecmp(a, b);
#else
    return !strcmp(a, b);
#endif
}

static bool load_last_session(struct mp_grid *grid, const char *filename,
                              bool *enabled, int *rows, int *columns,
                              double positions[MP_GRID_MAX_TILES],
                              int *num_positions)
{
    void *tmp = talloc_new(NULL);
    char *path = session_path(tmp);
    if (!path) {
        talloc_free(tmp);
        return false;
    }
    bstr data = stream_read_file(path, tmp, grid->mpctx->global, 256 * 1024);
    if (!data.start) {
        talloc_free(tmp);
        return false;
    }
    char *cursor = bstrto0(tmp, data);
    struct mpv_node root = {0};
    int version = 0;
    if (json_parse(tmp, &root, &cursor, MAX_JSON_DEPTH) < 0 ||
        root.format != MPV_FORMAT_NODE_MAP ||
        !node_string(&root, "format") ||
        strcmp(node_string(&root, "format"), GRID_SESSION_FORMAT) ||
        !node_int(&root, "version", &version) ||
        version != GRID_SESSION_VERSION)
    {
        talloc_free(tmp);
        return false;
    }
    struct mpv_node *valid = node_map_get(&root, "valid");
    if (valid && valid->format == MPV_FORMAT_FLAG && !valid->u.flag) {
        talloc_free(tmp);
        return false;
    }
    const char *saved_path = node_string(&root, "path");
    const char *mode = node_string(&root, "mode");
    char *normalized = mp_normalize_path(tmp, filename);
    if (!saved_path || !normalized || !paths_equal(saved_path, normalized) || !mode) {
        talloc_free(tmp);
        return false;
    }
    if (!strcmp(mode, "no")) {
        *enabled = false;
        *rows = *columns = 0;
    } else {
        *enabled = true;
        if (!parse_layout(mode, rows, columns)) {
            talloc_free(tmp);
            return false;
        }
    }
    struct mpv_node *array = node_map_get(&root, "positions");
    if (!array || array->format != MPV_FORMAT_NODE_ARRAY) {
        talloc_free(tmp);
        return false;
    }
    *num_positions = 0;
    int count = MPMIN(array->u.list->num, MP_GRID_MAX_TILES);
    for (int i = 0; i < count; i++) {
        struct mpv_node *entry = &array->u.list->values[i];
        double value = 0;
        if (entry->format == MPV_FORMAT_DOUBLE)
            value = entry->u.double_;
        else if (entry->format == MPV_FORMAT_INT64)
            value = entry->u.int64;
        else
            break;
        positions[(*num_positions)++] = MPMAX(0, value);
    }
    bool ok = *num_positions > 0;
    talloc_free(tmp);
    return ok;
}

static bool session_is_single_video(struct mp_grid *grid)
{
    struct MPContext *mpctx = grid->mpctx;
    if (!mpctx->playback_initialized || !mpctx->filename ||
        !mpctx->playlist || mpctx->playlist->num_entries != 1)
        return false;
    mp_mutex_lock(&grid->lock);
    bool single = !grid->project_path &&
                  (!grid->enabled || grid->single_source_auto);
    mp_mutex_unlock(&grid->lock);
    return single;
}

static void invalidate_last_session(struct mp_grid *grid)
{
    if (!grid || grid->session_invalidated)
        return;
    char *path = session_path(NULL);
    if (!path)
        return;
    bstr dir = mp_dirname(path);
    char *dir0 = bstrto0(NULL, dir);
    mp_mkdirp(dir0);
    talloc_free(dir0);
    struct mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_string(&root, "format", GRID_SESSION_FORMAT);
    node_map_add_int64(&root, "version", GRID_SESSION_VERSION);
    node_map_add_flag(&root, "valid", false);
    char *json = NULL;
    bool ok = json_write_pretty(&json, &root) >= 0 && json &&
              mp_save_to_file(path, json, strlen(json));
    talloc_free(json);
    talloc_free(root.u.list);
    talloc_free(path);
    if (ok)
        grid->session_invalidated = true;
}

static void save_last_session(struct mp_grid *grid, bool eof)
{
    if (!grid)
        return;
    if (!session_is_single_video(grid)) {
        invalidate_last_session(grid);
        return;
    }

    struct MPContext *mpctx = grid->mpctx;
    double main_position = eof ? 0 : get_current_time(mpctx);
    if (main_position == MP_NOPTS_VALUE)
        return;
    char *normalized = mp_normalize_path(NULL, mpctx->filename);
    char *path = session_path(NULL);
    if (!normalized || !path) {
        talloc_free(normalized);
        talloc_free(path);
        return;
    }
    bstr dir = mp_dirname(path);
    char *dir0 = bstrto0(NULL, dir);
    mp_mkdirp(dir0);
    talloc_free(dir0);

    struct mpv_node root;
    node_init(&root, MPV_FORMAT_NODE_MAP, NULL);
    node_map_add_string(&root, "format", GRID_SESSION_FORMAT);
    node_map_add_int64(&root, "version", GRID_SESSION_VERSION);
    node_map_add_flag(&root, "valid", true);
    node_map_add_string(&root, "path", normalized);

    mp_mutex_lock(&grid->lock);
    bool enabled = grid->enabled;
    int count = enabled ? layout_count(grid) : 1;
    char mode[16];
    if (enabled)
        snprintf(mode, sizeof(mode), "%dx%d", grid->rows, grid->columns);
    else
        snprintf(mode, sizeof(mode), "no");
    node_map_add_string(&root, "mode", mode);
    struct mpv_node *positions = node_map_add(&root, "positions",
                                              MPV_FORMAT_NODE_ARRAY);
    int64_t now = mp_time_ns();
    for (int i = 0; i < count; i++) {
        struct mpv_node *entry = node_array_add(positions, MPV_FORMAT_DOUBLE);
        entry->u.double_ = eof ? 0 : i == 0 ? main_position :
                           tile_clock_locked(&grid->tiles[i], now);
    }
    mp_mutex_unlock(&grid->lock);

    char *json = NULL;
    bool ok = json_write_pretty(&json, &root) >= 0 && json &&
              mp_save_to_file(path, json, strlen(json));
    talloc_free(json);
    talloc_free(root.u.list);
    talloc_free(normalized);
    talloc_free(path);
    if (ok) {
        grid->last_session_save_ns = now;
        grid->session_invalidated = false;
    }
}

static void clear_model_locked(struct mp_grid *grid)
{
    for (int i = 0; i < MP_GRID_MAX_TILES; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        talloc_free(tile->items);
        tile->items = NULL;
        tile->num_items = 0;
        tile->current_index = -1;
        tile->decoder.command_serial++;
        tile->decoder.reload = true;
    }
    grid->playback_snapshot = false;
}

static uint64_t get_le64(const uint8_t *data)
{
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--)
        value = (value << 8) | data[i];
    return value;
}

static uint32_t get_le32(const uint8_t *data)
{
    return data[0] | (uint32_t)data[1] << 8 |
           (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

static void put_le64(uint8_t *data, uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        data[i] = value;
        value >>= 8;
    }
}

static bool project_json_from_data(bstr data, bstr *json)
{
    if (!data.start || !data.len)
        return false;
    if (data.start[0] == '{') {
        *json = data;
        return true;
    }
    if (data.len < GRID_CONTAINER_TRAILER_SIZE)
        return false;
    const uint8_t *trailer = (uint8_t *)data.start + data.len -
                             GRID_CONTAINER_TRAILER_SIZE;
    if (memcmp(trailer, GRID_CONTAINER_MAGIC, GRID_CONTAINER_MAGIC_SIZE))
        return false;
    uint64_t json_size = get_le64(trailer + GRID_CONTAINER_MAGIC_SIZE);
    if (!json_size || json_size > data.len - GRID_CONTAINER_TRAILER_SIZE)
        return false;
    json->start = data.start + data.len - GRID_CONTAINER_TRAILER_SIZE - json_size;
    json->len = json_size;
    return json->start[0] == '{';
}

bool mp_grid_load_project(struct mp_grid *grid, const char *path)
{
    void *tmp = talloc_new(NULL);
    bstr data = stream_read_file(path, tmp, grid->mpctx->global, 16 * 1024 * 1024);
    if (!data.start) {
        MP_ERR(grid, "Cannot read grid project: %s\n", path);
        talloc_free(tmp);
        return false;
    }
    bstr json_data = {0};
    if (!project_json_from_data(data, &json_data)) {
        MP_ERR(grid, "Invalid grid project container: %s\n", path);
        talloc_free(tmp);
        return false;
    }
    char *text = bstrto0(tmp, json_data);
    char *cursor = text;
    struct mpv_node root = {0};
    if (json_parse(tmp, &root, &cursor, MAX_JSON_DEPTH) < 0 ||
        root.format != MPV_FORMAT_NODE_MAP ||
        !node_string(&root, "format") || strcmp(node_string(&root, "format"), GRID_FORMAT))
    {
        MP_ERR(grid, "Invalid .mpvgrid project: %s\n", path);
        talloc_free(tmp);
        return false;
    }
    int version = 0;
    if (!node_int(&root, "version", &version) || version != GRID_VERSION) {
        MP_ERR(grid, "Unsupported .mpvgrid version in %s\n", path);
        talloc_free(tmp);
        return false;
    }
    char *project_abs = mp_normalize_path(tmp, path);
    if (!project_abs) {
        MP_ERR(grid, "Cannot resolve grid project path: %s\n", path);
        talloc_free(tmp);
        return false;
    }
    bstr project_dir = mp_dirname(project_abs);
    bool project_enabled = true;
    struct mpv_node *enabled = node_map_get(&root, "enabled");
    if (enabled && enabled->format == MPV_FORMAT_FLAG)
        project_enabled = enabled->u.flag;
    struct mpv_node *layout = node_map_get(&root, "layout");
    int rows = 0, columns = 0;
    if (!layout || layout->format != MPV_FORMAT_NODE_MAP ||
        !node_int(layout, "rows", &rows) || !node_int(layout, "columns", &columns) ||
        (project_enabled ? !valid_layout(rows, columns)
                         : rows != 1 || columns != 1))
    {
        MP_ERR(grid, "Invalid grid layout in %s\n", path);
        talloc_free(tmp);
        return false;
    }

    mp_mutex_lock(&grid->lock);
    clear_model_locked(grid);
    grid->enabled = project_enabled;
    grid->rows = rows;
    grid->columns = columns;
    node_int(&root, "activeTile", &grid->active_tile);
    grid->active_tile = MPCLAMP(grid->active_tile, 0, layout_count(grid) - 1);
    const char *mode = node_string(&root, "openMode");
    grid->open_mode = mode && !strcmp(mode, "resume") ? MP_GRID_OPEN_RESUME :
                      mode && !strcmp(mode, "fixed") ? MP_GRID_OPEN_FIXED : MP_GRID_OPEN_ASK;
    struct mpv_node *snapshot = node_map_get(&root, "playbackSnapshot");
    const char *extension = strrchr(path, '.');
    bool snapshot_marker = snapshot && snapshot->format == MPV_FORMAT_FLAG &&
                           snapshot->u.flag;
    bool grd_container = extension && !strcasecmp(extension, ".grd");
    bool legacy_snapshot = grd_container && !snapshot_marker;
    grid->playback_snapshot = snapshot_marker || grd_container;
    talloc_free(grid->project_path);
    grid->project_path = talloc_strdup(grid, project_abs);

    struct mpv_node *tiles = node_map_get(&root, "tiles");
    if (tiles && tiles->format == MPV_FORMAT_NODE_ARRAY) {
        int num = MPMIN(tiles->u.list->num, MP_GRID_MAX_TILES);
        for (int i = 0; i < num; i++) {
            struct mpv_node *tn = &tiles->u.list->values[i];
            if (tn->format != MPV_FORMAT_NODE_MAP) continue;
            struct grid_tile *tile = &grid->tiles[i];
            node_int(tn, "currentIndex", &tile->current_index);
            double d;
            if (node_number(tn, "speed", &d)) tile->speed = MPCLAMP(d, 0.1, 8);
            struct mpv_node *paused = node_map_get(tn, "paused");
            if (paused && paused->format == MPV_FORMAT_FLAG) tile->paused = paused->u.flag;
            struct mpv_node *audio = node_map_get(tn, "audio");
            if (audio && audio->format == MPV_FORMAT_NODE_MAP) {
                struct mpv_node *muted = node_map_get(audio, "muted");
                if (muted && muted->format == MPV_FORMAT_FLAG) tile->muted = muted->u.flag;
                if (node_number(audio, "volume", &d)) tile->volume = MPCLAMP(d, 0, 2);
            }
            struct mpv_node *playlist = node_map_get(tn, "playlist");
            if (!playlist || playlist->format != MPV_FORMAT_NODE_ARRAY) continue;
            for (int j = 0; j < playlist->u.list->num; j++) {
                struct mpv_node *in = &playlist->u.list->values[j];
                if (in->format != MPV_FORMAT_NODE_MAP) continue;
                const char *rel = node_string(in, "path");
                const char *hint = node_string(in, "absolutePathHint");
                if (!rel && !hint) continue;
                char *candidate = rel ? mp_path_join_bstr(tmp, project_dir, bstr0(rel)) : NULL;
                char *normalized_candidate = candidate
                                                ? mp_normalize_path(tmp, candidate)
                                                : NULL;
                const char *resolved = normalized_candidate &&
                                       mp_path_exists(normalized_candidate)
                                        ? normalized_candidate : hint;
                if (!resolved)
                    resolved = normalized_candidate ? normalized_candidate : hint;
                MP_TARRAY_GROW(grid, tile->items, tile->num_items);
                struct grid_item *item = &tile->items[tile->num_items++];
                item->path = mp_normalize_path(tile->items, resolved);
                item->absolute_hint = hint
                                        ? mp_normalize_path(tile->items, hint)
                                        : talloc_strdup(tile->items, item->path);
                node_number(in, "fixedStart", &item->fixed_start);
                if (!node_number(in, "resumePosition", &item->resume_position))
                    item->resume_position = item->fixed_start;
                // Before playbackSnapshot existed, F1 stored its captured
                // timestamp only as resumePosition. Promote that value once
                // so old desktop links retain the moment the user bookmarked.
                if (legacy_snapshot)
                    item->fixed_start = item->resume_position;
                item->opened_resume_position = item->resume_position;
            }
            if (!tile->num_items) tile->current_index = -1;
            else tile->current_index = MPCLAMP(tile->current_index, 0, tile->num_items - 1);
            struct grid_item *item = current_item(tile);
            if (item) set_tile_clock_locked(tile,
                grid->open_mode == MP_GRID_OPEN_RESUME ? item->resume_position : item->fixed_start,
                mp_time_ns());
        }
    }
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);
    talloc_free(tmp);
    return true;
}

static const char *open_mode_name(enum mp_grid_open_mode mode)
{
    return mode == MP_GRID_OPEN_FIXED ? "fixed" :
           mode == MP_GRID_OPEN_RESUME ? "resume" : "ask";
}

static bool same_component(const char *a, const char *b)
{
#if HAVE_DOS_PATHS
    return !strcasecmp(a, b);
#else
    return !strcmp(a, b);
#endif
}

// Return a portable '/' separated path relative to base_dir. Both inputs are
// expected to be normalized absolute paths. If Windows roots differ, retain
// the absolute path so the absolute hint remains usable.
static char *relative_media_path(void *ta_parent, const char *base_dir,
                                 const char *media_path)
{
    char *base = talloc_strdup(NULL, base_dir);
    char *media = talloc_strdup(NULL, media_path);
    for (char *p = base; *p; p++) if (*p == '\\') *p = '/';
    for (char *p = media; *p; p++) if (*p == '\\') *p = '/';
#if HAVE_DOS_PATHS
    if ((strlen(base) >= 2 && base[1] == ':') !=
        (strlen(media) >= 2 && media[1] == ':') ||
        (strlen(base) >= 2 && base[1] == ':' &&
         tolower((unsigned char)base[0]) != tolower((unsigned char)media[0])))
    {
        talloc_free(base);
        char *result = talloc_steal(ta_parent, media);
        return result;
    }
#endif
    char *base_parts[256] = {0};
    char *media_parts[256] = {0};
    int base_count = 0, media_count = 0;
    for (char *p = base; *p && base_count < MP_ARRAY_SIZE(base_parts);) {
        while (*p == '/') p++;
        if (!*p) break;
        base_parts[base_count++] = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = '\0';
    }
    for (char *p = media; *p && media_count < MP_ARRAY_SIZE(media_parts);) {
        while (*p == '/') p++;
        if (!*p) break;
        media_parts[media_count++] = p;
        while (*p && *p != '/') p++;
        if (*p) *p++ = '\0';
    }
    int common = 0;
    while (common < base_count && common < media_count &&
           same_component(base_parts[common], media_parts[common]))
        common++;
#if HAVE_DOS_PATHS
    if (!common) {
        talloc_free(base);
        char *result = mp_normalize_path(ta_parent, media_path);
        talloc_free(media);
        return result;
    }
#endif
    char *result = talloc_strdup(ta_parent, "");
    for (int i = common; i < base_count; i++)
        result = talloc_strdup_append(result, "../");
    for (int i = common; i < media_count; i++) {
        if (result[0] && result[strlen(result) - 1] != '/')
            result = talloc_strdup_append(result, "/");
        result = talloc_strdup_append(result, media_parts[i]);
    }
    if (!result[0])
        result = talloc_strdup_append(result, ".");
    talloc_free(base);
    talloc_free(media);
    return result;
}

static void grid_to_node(struct mp_grid *grid, struct mpv_node *root,
                         const char *project_target)
{
    node_init(root, MPV_FORMAT_NODE_MAP, NULL);
    void *tmp = talloc_new(NULL);
    char *project_abs = project_target
                            ? mp_normalize_path(tmp, project_target) : NULL;
    char *project_dir = project_abs
                            ? bstrto0(tmp, mp_dirname(project_abs)) : NULL;
    mp_mutex_lock(&grid->lock);
    node_map_add_flag(root, "enabled", grid->enabled);
    node_map_add_string(root, "format", GRID_FORMAT);
    node_map_add_int64(root, "version", GRID_VERSION);
    struct mpv_node *layout = node_map_add(root, "layout", MPV_FORMAT_NODE_MAP);
    node_map_add_int64(layout, "rows", grid->rows);
    node_map_add_int64(layout, "columns", grid->columns);
    node_map_add_int64(root, "activeTile", grid->active_tile);
    node_map_add_string(root, "openMode", open_mode_name(grid->open_mode));
    node_map_add_flag(root, "playbackSnapshot", grid->playback_snapshot);
    if (!project_target) {
        node_map_add_int64(root, "audioFifoLimit", grid->audio_fifo_limit);
        struct mpv_node *fifo = node_map_add(root, "audioFifo",
                                             MPV_FORMAT_NODE_ARRAY);
        for (int i = 0; i < grid->audio_fifo_count; i++) {
            struct mpv_node *entry = node_array_add(fifo, MPV_FORMAT_INT64);
            entry->u.int64 = grid->audio_fifo[i];
        }
    }
    struct mpv_node *tiles = node_map_add(root, "tiles", MPV_FORMAT_NODE_ARRAY);
    for (int i = 0; i < MP_GRID_MAX_TILES; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        struct mpv_node *tn = node_array_add(tiles, MPV_FORMAT_NODE_MAP);
        node_map_add_int64(tn, "id", i);
        node_map_add_int64(tn, "currentIndex", tile->current_index);
        node_map_add_flag(tn, "paused", tile->paused);
        node_map_add_double(tn, "speed", tile->speed);
        struct mpv_node *audio = node_map_add(tn, "audio", MPV_FORMAT_NODE_MAP);
        node_map_add_flag(audio, "muted", tile->muted);
        node_map_add_double(audio, "volume", tile->volume);
        if (!project_target) {
            node_map_add_flag(audio, "fifoSelected",
                              audio_fifo_contains_locked(grid, i));
            node_map_add_int64(audio, "bufferedSamples",
                               tile->decoder.audio_count);
            node_map_add_int64(audio, "mixedSamples",
                               tile->audio_mixed_samples);
            if (i > 0) {
                node_map_add_double(audio, "readPosition",
                                    tile->decoder.audio_read_position);
                node_map_add_flag(audio, "syncPending",
                                  tile->decoder.audio_resync);
                node_map_add_int64(audio, "prerollDroppedSamples",
                                   tile->decoder.audio_preroll_dropped);
                node_map_add_int64(audio, "syncDroppedSamples",
                                   tile->decoder.audio_sync_dropped);
            }
        }
        node_map_add_flag(tn, "subtitles", tile->subtitles);
        node_map_add_string(tn, "status", tile->error ? tile->error :
                            tile->num_items ? "ready" : "empty");
        node_map_add_double(tn, "position", tile_clock_locked(tile, mp_time_ns()));
        struct mpv_node *playlist = node_map_add(tn, "playlist", MPV_FORMAT_NODE_ARRAY);
        for (int j = 0; j < tile->num_items; j++) {
            struct grid_item *item = &tile->items[j];
            struct mpv_node *in = node_array_add(playlist, MPV_FORMAT_NODE_MAP);
            char *absolute = mp_normalize_path(tmp, item->path);
            char *relative = project_dir && absolute &&
                             !mp_is_url(bstr0(absolute))
                                ? relative_media_path(tmp, project_dir, absolute)
                                : NULL;
            node_map_add_string(in, "path", relative ? relative : item->path);
            node_map_add_string(in, "absolutePathHint",
                                absolute ? absolute : item->absolute_hint);
            node_map_add_double(in, "fixedStart", item->fixed_start);
            node_map_add_double(in, "resumePosition", item->resume_position);
        }
    }
    mp_mutex_unlock(&grid->lock);
    talloc_free(tmp);
}

void mp_grid_to_node(struct mp_grid *grid, struct mpv_node *root)
{
    grid_to_node(grid, root, NULL);
}

static bool path_has_grd_extension(const char *path)
{
    const char *extension = strrchr(path, '.');
    return extension && !strcasecmp(extension, ".grd");
}

static bool save_project_json(struct mp_grid *grid, const char *path,
                              const char *json)
{
    if (!path_has_grd_extension(path))
        return mp_save_to_file(path, json, strlen(json));

    // Preserve the ICO prefix of an existing .grd while atomically replacing
    // its project payload. This keeps the per-file Explorer thumbnail intact
    // when the regular five-second resume checkpoint is written.
    void *tmp = talloc_new(NULL);
    bstr previous = stream_read_file(path, tmp, grid->mpctx->global,
                                     32 * 1024 * 1024);
    if (!previous.start || previous.len < 22 || previous.start[0] != 0 ||
        previous.start[1] != 0 || previous.start[2] != 1 ||
        previous.start[3] != 0)
    {
        MP_ERR(grid, "Cannot update .grd without a valid embedded icon: %s\n",
               path);
        talloc_free(tmp);
        return false;
    }
    const uint8_t *bytes = (uint8_t *)previous.start;
    uint32_t image_size = get_le32(bytes + 14);
    uint32_t image_offset = get_le32(bytes + 18);
    uint64_t icon_size = (uint64_t)image_offset + image_size;
    if (image_offset < 22 || icon_size > previous.len) {
        MP_ERR(grid, "Invalid embedded icon in .grd: %s\n", path);
        talloc_free(tmp);
        return false;
    }
    size_t json_size = strlen(json);
    size_t total = icon_size + json_size + GRID_CONTAINER_TRAILER_SIZE;
    uint8_t *container = talloc_array(tmp, uint8_t, total);
    memcpy(container, previous.start, icon_size);
    memcpy(container + icon_size, json, json_size);
    uint8_t *trailer = container + total - GRID_CONTAINER_TRAILER_SIZE;
    memcpy(trailer, GRID_CONTAINER_MAGIC, GRID_CONTAINER_MAGIC_SIZE);
    put_le64(trailer + GRID_CONTAINER_MAGIC_SIZE, json_size);
    bool ok = mp_save_to_file(path, container, total);
    talloc_free(tmp);
    return ok;
}

bool mp_grid_save_project(struct mp_grid *grid, const char *path,
                          bool update_fixed_positions)
{
    if (!grid) return false;
    char *target = NULL;
    mp_mutex_lock(&grid->lock);
    if (path && path[0])
        target = talloc_strdup(NULL, path);
    else if (grid->project_path)
        target = talloc_strdup(NULL, grid->project_path);
    else {
        struct grid_item *item = current_item(&grid->tiles[0]);
        if (item && item->path)
            target = talloc_asprintf(NULL, "%s.mpvgrid", item->path);
    }
    mp_mutex_unlock(&grid->lock);
    if (!target)
        return false;
    mp_grid_update_resume(grid);
    if (update_fixed_positions) {
        mp_mutex_lock(&grid->lock);
        for (int i = 0; i < MP_GRID_MAX_TILES; i++) {
            struct grid_item *item = current_item(&grid->tiles[i]);
            if (item) item->fixed_start = grid->tiles[i].position;
        }
        mp_mutex_unlock(&grid->lock);
    }
    struct mpv_node node;
    grid_to_node(grid, &node, target);
    char *json = NULL;
    bool ok = json_write_pretty(&json, &node) >= 0 && json &&
              save_project_json(grid, target, json);
    talloc_free(json);
    talloc_free(node.u.list);
    if (ok) {
        talloc_free(grid->project_path);
        grid->project_path = mp_normalize_path(grid, target);
    } else {
        MP_ERR(grid, "Could not atomically save grid project: %s\n", target);
    }
    talloc_free(target);
    return ok;
}

#if HAVE_WIN32_DESKTOP
static char *desktop_project_json(struct mp_grid *grid, void *ta_parent,
                                  const char *path, const char *media,
                                  double position, bool enabled)
{
    mp_grid_update_resume(grid);
    struct mpv_node node;
    grid_to_node(grid, &node, path);
    struct mpv_node *mode = node_map_get(&node, "openMode");
    if (mode && mode->format == MPV_FORMAT_STRING) {
        talloc_free(mode->u.string);
        mode->u.string = talloc_strdup(node.u.list, "fixed");
    }
    struct mpv_node *snapshot = node_map_get(&node, "playbackSnapshot");
    if (snapshot && snapshot->format == MPV_FORMAT_FLAG)
        snapshot->u.flag = true;
    else
        node_map_add_flag(&node, "playbackSnapshot", true);

    // F1 bookmarks the complete scene, so every currently selected media
    // item starts at the timestamp visible in its tile when the key was
    // pressed. Non-current queue entries keep their independent positions.
    struct mpv_node *all_tiles = node_map_get(&node, "tiles");
    int visible = enabled ? layout_count(grid) : 1;
    if (all_tiles && all_tiles->format == MPV_FORMAT_NODE_ARRAY) {
        visible = MPMIN(visible, all_tiles->u.list->num);
        for (int i = 0; i < visible; i++) {
            struct mpv_node *tile = &all_tiles->u.list->values[i];
            struct mpv_node *playlist = tile->format == MPV_FORMAT_NODE_MAP
                                        ? node_map_get(tile, "playlist") : NULL;
            struct mpv_node *current = tile->format == MPV_FORMAT_NODE_MAP
                                        ? node_map_get(tile, "currentIndex") : NULL;
            struct mpv_node *tile_position = tile->format == MPV_FORMAT_NODE_MAP
                                        ? node_map_get(tile, "position") : NULL;
            int index = current && current->format == MPV_FORMAT_INT64
                        ? current->u.int64 : -1;
            double captured = i == 0 && !enabled ? position : 0;
            if (tile_position) {
                if (tile_position->format == MPV_FORMAT_DOUBLE)
                    captured = tile_position->u.double_;
                else if (tile_position->format == MPV_FORMAT_INT64)
                    captured = tile_position->u.int64;
            }
            if (!playlist || playlist->format != MPV_FORMAT_NODE_ARRAY ||
                index < 0 || index >= playlist->u.list->num)
                continue;
            struct mpv_node *item = &playlist->u.list->values[index];
            if (item->format != MPV_FORMAT_NODE_MAP)
                continue;
            struct mpv_node *fixed = node_map_get(item, "fixedStart");
            struct mpv_node *resume = node_map_get(item, "resumePosition");
            if (fixed && fixed->format == MPV_FORMAT_DOUBLE)
                fixed->u.double_ = captured;
            else
                node_map_add_double(item, "fixedStart", captured);
            if (resume && resume->format == MPV_FORMAT_DOUBLE)
                resume->u.double_ = captured;
            else
                node_map_add_double(item, "resumePosition", captured);
        }
    }
    if (!enabled) {
        struct mpv_node *enabled_node = node_map_get(&node, "enabled");
        struct mpv_node *layout = node_map_get(&node, "layout");
        struct mpv_node *active = node_map_get(&node, "activeTile");
        if (enabled_node && enabled_node->format == MPV_FORMAT_FLAG)
            enabled_node->u.flag = false;
        struct mpv_node *rows = layout ? node_map_get(layout, "rows") : NULL;
        struct mpv_node *columns = layout ? node_map_get(layout, "columns") : NULL;
        if (rows && rows->format == MPV_FORMAT_INT64)
            rows->u.int64 = 1;
        if (columns && columns->format == MPV_FORMAT_INT64)
            columns->u.int64 = 1;
        if (active && active->format == MPV_FORMAT_INT64)
            active->u.int64 = 0;

        struct mpv_node *tiles = node_map_get(&node, "tiles");
        struct mpv_node *tile = tiles && tiles->format == MPV_FORMAT_NODE_ARRAY &&
                               tiles->u.list->num
                                ? &tiles->u.list->values[0] : NULL;
        struct mpv_node *playlist = tile ? node_map_get(tile, "playlist") : NULL;
        struct mpv_node *current = tile ? node_map_get(tile, "currentIndex") : NULL;
        int index = current && current->format == MPV_FORMAT_INT64
                    ? current->u.int64 : -1;
        if (playlist && playlist->format == MPV_FORMAT_NODE_ARRAY &&
            index >= 0 && index < playlist->u.list->num)
        {
            struct mpv_node *item = &playlist->u.list->values[index];
            struct mpv_node *resume = item->format == MPV_FORMAT_NODE_MAP
                                        ? node_map_get(item, "resumePosition") : NULL;
            if (resume && resume->format == MPV_FORMAT_DOUBLE)
                resume->u.double_ = position;
            else if (item->format == MPV_FORMAT_NODE_MAP)
                node_map_add_double(item, "resumePosition", position);
        } else if (playlist && playlist->format == MPV_FORMAT_NODE_ARRAY) {
            if (current && current->format == MPV_FORMAT_INT64)
                current->u.int64 = 0;
            struct mpv_node *item = node_array_add(playlist, MPV_FORMAT_NODE_MAP);
            void *tmp = talloc_new(NULL);
            char *absolute = mp_normalize_path(tmp, media);
            char *project_abs = mp_normalize_path(tmp, path);
            char *project_dir = project_abs
                                ? bstrto0(tmp, mp_dirname(project_abs)) : NULL;
            char *relative = absolute && project_dir
                             ? relative_media_path(tmp, project_dir, absolute)
                             : NULL;
            node_map_add_string(item, "path", relative ? relative : media);
            node_map_add_string(item, "absolutePathHint",
                                absolute ? absolute : media);
            node_map_add_double(item, "fixedStart", position);
            node_map_add_double(item, "resumePosition", position);
            talloc_free(tmp);
        }
    }
    char *json = NULL;
    if (json_write_pretty(&json, &node) < 0 || !json)
        json = NULL;
    else
        json = talloc_steal(ta_parent, json);
    talloc_free(node.u.list);
    return json;
}

static struct mp_rect shortcut_source_rect(struct mp_image *image, double zoom,
                                           double center_x, double center_y)
{
    struct mp_rect source = {0, 0, image->params.w, image->params.h};
    if (mp_image_crop_valid(&image->params))
        source = image->params.crop;
    zoom = MPCLAMP(zoom, 1.0, 8.0);
    if (zoom > 1.0) {
        int full_w = mp_rect_w(source), full_h = mp_rect_h(source);
        int view_w = MPMAX(2, full_w / zoom);
        int view_h = MPMAX(2, full_h / zoom);
        int cx = source.x0 + MPCLAMP(center_x, 0.0, 1.0) * full_w;
        int cy = source.y0 + MPCLAMP(center_y, 0.0, 1.0) * full_h;
        source.x0 = MPCLAMP(cx - view_w / 2, source.x0, source.x1 - view_w);
        source.y0 = MPCLAMP(cy - view_h / 2, source.y0, source.y1 - view_h);
        source.x1 = source.x0 + view_w;
        source.y1 = source.y0 + view_h;
    }
    return source;
}

static bool save_shortcut_thumbnail(struct mp_grid *grid, const char *path)
{
    const int size = 256;
    struct mp_image *thumbnail = mp_image_alloc(IMGFMT_BGRA, size, size);
    if (!thumbnail)
        return false;
    mp_image_clear(thumbnail, 0, 0, size, size);
    for (int y = 0; y < size; y++) {
        uint8_t *line = thumbnail->planes[0] + y * thumbnail->stride[0];
        for (int x = 0; x < size; x++)
            line[x * 4 + 3] = 255;
    }

    struct mp_grid_snapshot snapshot = {0};
    bool enabled = mp_grid_snapshot(grid, &snapshot);
    int rows = enabled ? snapshot.rows : 1;
    int columns = enabled ? snapshot.columns : 1;
    int cells = enabled ? snapshot.num_cells : 1;
    struct vo *vo = NULL;
    mp_mutex_lock(&grid->lock);
    vo = grid->vo;
    mp_mutex_unlock(&grid->lock);
    struct mp_image *main_image = vo ? vo_get_current_frame(vo) : NULL;
    struct mp_image *aspect_image = main_image;
    if (!aspect_image && enabled) {
        for (int i = 1; i < cells && !aspect_image; i++)
            aspect_image = snapshot.cells[i].image;
    }
    int aspect_w = 16, aspect_h = 9;
    if (aspect_image)
        mp_image_params_get_dsize(&aspect_image->params, &aspect_w, &aspect_h);
    if (aspect_w <= 0 || aspect_h <= 0) {
        aspect_w = 16;
        aspect_h = 9;
    }
    // Shortcut thumbnails should still use the available icon surface. The
    // no-upscale rule applies to playback pixels, not to this derived preview.
    struct mp_rect canvas = grid_calc_output_rect(
        size, size, rows, columns, aspect_w, aspect_h, true);
    struct mp_sws_context *sws = mp_sws_alloc(NULL);
    sws->log = grid->log;
    bool scaled = false;
    for (int i = 0; i < cells; i++) {
        struct mp_image *frame = i == 0 ? main_image : snapshot.cells[i].image;
        if (!frame)
            continue;
        struct mp_image *source = mp_image_new_ref(frame);
        if (source && IMGFMT_IS_HWACCEL(source->imgfmt)) {
            struct mp_image *downloaded = mp_image_hw_download(source, NULL);
            talloc_free(source);
            source = downloaded;
        }
        if (!source)
            continue;

        int row = i / columns, column = i % columns;
        struct mp_rect cell = {
            .x0 = canvas.x0 + mp_rect_w(canvas) * column / columns,
            .y0 = canvas.y0 + mp_rect_h(canvas) * row / rows,
            .x1 = canvas.x0 + mp_rect_w(canvas) * (column + 1) / columns,
            .y1 = canvas.y0 + mp_rect_h(canvas) * (row + 1) / rows,
        };
        int source_w = 0, source_h = 0;
        mp_image_params_get_dsize(&source->params, &source_w, &source_h);
        if (source_w <= 0 || source_h <= 0) {
            source_w = source->w;
            source_h = source->h;
        }
        int cell_w = mp_rect_w(cell), cell_h = mp_rect_h(cell);
        double scale = MPMIN((double)cell_w / source_w,
                             (double)cell_h / source_h);
        int width = MPCLAMP(lrint(source_w * scale), 1, cell_w);
        int height = MPCLAMP(lrint(source_h * scale), 1, cell_h);
        cell.x0 += (cell_w - width) / 2;
        cell.y0 += (cell_h - height) / 2;
        cell.x1 = cell.x0 + width;
        cell.y1 = cell.y0 + height;
        struct mp_image source_view = *source;
        if (enabled) {
            struct mp_grid_snapshot_cell *state = &snapshot.cells[i];
            mp_image_crop_rc(&source_view,
                shortcut_source_rect(source, state->zoom,
                                     state->center_x, state->center_y));
        }
        struct mp_image target = *thumbnail;
        mp_image_crop_rc(&target, cell);
        if (mp_sws_scale(sws, &target, &source_view) >= 0)
            scaled = true;
        talloc_free(source);
    }
    talloc_free(sws);
    talloc_free(main_image);
    mp_grid_snapshot_free(&snapshot);

    struct image_writer_opts opts = image_writer_opts_defaults;
    opts.format = AV_CODEC_ID_PNG;
    opts.high_bit_depth = false;
    opts.tag_csp = false;
    bool written = scaled && write_image(thumbnail, &opts, path,
                                         grid->mpctx->global, grid->log, true);
    talloc_free(thumbnail);
    return written;
}

static void put_le16(uint8_t *data, uint16_t value)
{
    data[0] = value;
    data[1] = value >> 8;
}

static void put_le32(uint8_t *data, uint32_t value)
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static bool save_grd_container(struct mp_grid *grid, const char *png_path,
                               const char *grd_path, const char *icon_path,
                               const char *json)
{
    void *tmp = talloc_new(NULL);
    bstr png = stream_read_file(png_path, tmp, grid->mpctx->global,
                                4 * 1024 * 1024);
    if (!png.start || png.len > UINT32_MAX) {
        talloc_free(tmp);
        return false;
    }
    size_t json_size = strlen(json);
    const size_t header_size = 22;
    size_t total = header_size + png.len + json_size +
                   GRID_CONTAINER_TRAILER_SIZE;
    uint8_t *container = talloc_zero_array(tmp, uint8_t, total);
    put_le16(container + 2, 1);
    put_le16(container + 4, 1);
    // Width and height values of zero represent a 256x256 PNG icon.
    put_le16(container + 10, 1);
    put_le16(container + 12, 32);
    put_le32(container + 14, png.len);
    put_le32(container + 18, header_size);
    memcpy(container + header_size, png.start, png.len);
    memcpy(container + header_size + png.len, json, json_size);
    uint8_t *trailer = container + total - GRID_CONTAINER_TRAILER_SIZE;
    memcpy(trailer, GRID_CONTAINER_MAGIC, GRID_CONTAINER_MAGIC_SIZE);
    put_le64(trailer + GRID_CONTAINER_MAGIC_SIZE, json_size);
    bool ok = mp_save_to_file(icon_path, container, header_size + png.len) &&
              mp_save_to_file(grd_path, container, total);
    talloc_free(tmp);
    return ok;
}

static void sanitize_shortcut_name(char *name)
{
    for (char *p = name; *p; p++) {
        unsigned char c = *p;
        if (c < 0x20 || strchr("<>:\"/\\|?*", c))
            *p = '_';
    }
}
#endif

bool mp_grid_create_desktop_shortcut(struct mp_grid *grid,
                                     char **shortcut_path)
{
    if (shortcut_path)
        *shortcut_path = NULL;
#if !HAVE_WIN32_DESKTOP
    return false;
#else
    if (!grid || !grid->mpctx->playback_initialized ||
        !grid->mpctx->filename)
        return false;

    void *tmp = talloc_new(NULL);
    double main_position = get_current_time(grid->mpctx);
    mp_mutex_lock(&grid->lock);
    bool enabled = grid->enabled;
    int rows = enabled ? grid->rows : 1;
    int columns = enabled ? grid->columns : 1;
    int active = enabled ? grid->active_tile : 0;
    struct grid_item *item = current_item(&grid->tiles[active]);
    char *media = talloc_strdup(tmp, item && item->path
                                        ? item->path : grid->mpctx->filename);
    double position = enabled ? tile_clock_locked(&grid->tiles[active],
                                                   mp_time_ns())
                              : main_position;
    mp_mutex_unlock(&grid->lock);
    if (!media || position == MP_NOPTS_VALUE) {
        talloc_free(tmp);
        return false;
    }

    const char *local = getenv("LOCALAPPDATA");
    char *desktop = mp_w32_get_desktop_path(tmp);
    if (!local || !local[0] || !desktop) {
        talloc_free(tmp);
        return false;
    }
    char *storage = mp_path_join(tmp, local, "mpv-grid/shortcuts");
    mp_mkdirp(storage);
    if (!mp_path_exists(storage)) {
        talloc_free(tmp);
        return false;
    }
    char *id = talloc_asprintf(tmp, "%lld-%lld-%lu",
                               (long long)time(NULL),
                               (long long)mp_time_ns(),
                               (unsigned long)mp_getpid());
    char *png_path = mp_path_join(tmp, storage,
                                  talloc_asprintf(tmp, "%s.png", id));
    char *icon_path = mp_path_join(tmp, storage,
                                   talloc_asprintf(tmp, "%s.ico", id));
    char *project_path = mp_path_join(tmp, storage,
                                      talloc_asprintf(tmp, "%s.grd", id));
    char *base = talloc_strdup(tmp, mp_basename(media));
    char *extension = strrchr(base, '.');
    if (extension && extension != base)
        *extension = '\0';
    sanitize_shortcut_name(base);
    int64_t seconds = MPMAX(0, llrint(position));
    char *label = talloc_asprintf(tmp, "%s - %dx%d - %02lld-%02lld-%02lld",
        base[0] ? base : "mpv Grid", rows, columns,
        (long long)(seconds / 3600), (long long)(seconds / 60 % 60),
        (long long)(seconds % 60));
    char *shortcut = mp_path_join(tmp, desktop,
                                  talloc_asprintf(tmp, "%s.lnk", label));
    for (int suffix = 2; mp_path_exists(shortcut); suffix++) {
        shortcut = mp_path_join(tmp, desktop,
            talloc_asprintf(tmp, "%s (%d).lnk", label, suffix));
    }

    char *json = desktop_project_json(grid, tmp, project_path, media, position,
                                      enabled);
    bool thumbnail = save_shortcut_thumbnail(grid, png_path);
    bool project = json && thumbnail &&
                   save_grd_container(grid, png_path, project_path,
                                      icon_path, json);
    unlink(png_path);
    bool ok = project &&
              mp_w32_create_shell_link(shortcut, project_path, NULL, storage,
                                       icon_path, "mpv Grid 播放现场");
    if (!ok) {
        unlink(project_path);
        unlink(icon_path);
    }
    if (ok && shortcut_path)
        *shortcut_path = talloc_strdup(NULL, shortcut);
    if (ok)
        MP_INFO(grid, "Desktop playback shortcut created: %s\n", shortcut);
    else
        MP_ERR(grid, "Could not create desktop playback shortcut.\n");
    talloc_free(tmp);
    return ok;
#endif
}

bool mp_grid_resume_project(struct mp_grid *grid, double *main_position)
{
    if (main_position)
        *main_position = MP_NOPTS_VALUE;
    if (!grid || !grid->project_path)
        return false;

    mp_mutex_lock(&grid->lock);
    int count = grid->enabled ? layout_count(grid) : 1;
    int64_t now = mp_time_ns();
    bool found = false;
    for (int i = 0; i < count; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        struct grid_item *item = current_item(tile);
        if (!item)
            continue;
        double position = MPMAX(0, item->opened_resume_position);
        tile->paused = false;
        set_tile_clock_locked(tile, position, now);
        tile->decoder.seek_target = position;
        tile->decoder.seek_pending = true;
        tile->decoder.command_serial++;
        if (i == 0 && main_position)
            *main_position = position;
        found = true;
    }
    mp_cond_broadcast(&grid->wakeup);
    mp_mutex_unlock(&grid->lock);
    if (found)
        mp_grid_request_redraw(grid);
    return found;
}

void mp_grid_update_resume(struct mp_grid *grid)
{
    if (!grid) return;
    mp_mutex_lock(&grid->lock);
    int64_t now = mp_time_ns();
    for (int i = 0; i < MP_GRID_MAX_TILES; i++) {
        struct grid_tile *tile = &grid->tiles[i];
        struct grid_item *item = current_item(tile);
        if (item) item->resume_position = tile_clock_locked(tile, now);
    }
    grid->last_autosave_ns = now;
    mp_mutex_unlock(&grid->lock);
}

void mp_grid_tick(struct mp_grid *grid)
{
    if (!grid)
        return;
    int64_t now = mp_time_ns();
    mp_mutex_lock(&grid->lock);
    bool enabled = grid->enabled;
    audio_fifo_reconcile_locked(grid, enabled ? layout_count(grid) : 1);
    bool playback_active = grid->media_active &&
                           grid->mpctx->playback_initialized;
    if (enabled && playback_active) {
        double position = get_current_time(grid->mpctx);
        if (position != MP_NOPTS_VALUE)
            set_tile_clock_locked(&grid->tiles[0], position, now);
    }
    bool session_due = playback_active &&
        now - grid->last_session_save_ns >= MP_TIME_S_TO_NS(5);
    bool project_due = enabled &&
        now - grid->last_autosave_ns >= MP_TIME_S_TO_NS(5);
    char *path = project_due && grid->project_path
                    ? talloc_strdup(NULL, grid->project_path) : NULL;
    mp_mutex_unlock(&grid->lock);
    if (session_due)
        save_last_session(grid, false);
    if (path) {
        mp_grid_save_project(grid, path, false);
        talloc_free(path);
    }
}
