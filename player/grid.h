/*
 * mpv-grid: single-core multi-pipeline grid playback support
 *
 * This file is part of mpv and is licensed under LGPLv2.1+.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MP_GRID_MAX_TILES 16

struct MPContext;
struct mp_chmap;
struct playlist;
struct mp_image;
struct mpv_node;
struct vo;

enum mp_grid_open_mode {
    MP_GRID_OPEN_ASK,
    MP_GRID_OPEN_FIXED,
    MP_GRID_OPEN_RESUME,
};

struct mp_grid_snapshot_cell {
    struct mp_image *image;
    int tile_id;
    bool selected;
    bool paused;
    bool muted;
    double position;
    double volume;
    double speed;
    double zoom;
    double center_x;
    double center_y;
};

struct mp_grid_snapshot {
    bool enabled;
    int rows;
    int columns;
    int aspect_w;
    int aspect_h;
    int num_cells;
    int active_tile;
    struct mp_grid_snapshot_cell cells[MP_GRID_MAX_TILES];
};

struct mp_grid;

struct mp_grid *mp_grid_create(struct MPContext *mpctx);
void mp_grid_prepare_destroy(struct mp_grid *grid);
void mp_grid_destroy(struct mp_grid *grid);

// Apply --grid/--grid-project after command-line parsing.
bool mp_grid_configure(struct mp_grid *grid);
char *mp_grid_main_path(struct mp_grid *grid, void *ta_parent);
bool mp_grid_populate_main_playlist(struct mp_grid *grid, struct playlist *playlist);
void mp_grid_tick(struct mp_grid *grid);

// Playback lifecycle hooks. The returned value is the generated start point
// for the normal mpv pipeline (tile 0), or MP_NOPTS_VALUE.
void mp_grid_file_started(struct mp_grid *grid);
double mp_grid_file_loaded(struct mp_grid *grid, const char *filename,
                           double duration, struct vo *vo);
void mp_grid_file_unloading(struct mp_grid *grid, bool eof);

bool mp_grid_enabled(struct mp_grid *grid);
bool mp_grid_empty(struct mp_grid *grid);
bool mp_grid_snapshot(struct mp_grid *grid, struct mp_grid_snapshot *snapshot);
void mp_grid_snapshot_free(struct mp_grid_snapshot *snapshot);

// Commands/properties exposed through player/command.c.
bool mp_grid_disable(struct mp_grid *grid);
bool mp_grid_set_layout(struct mp_grid *grid, int rows, int columns);
bool mp_grid_set_active(struct mp_grid *grid, int tile);
int mp_grid_get_active(struct mp_grid *grid);
bool mp_grid_cycle_active(struct mp_grid *grid, int direction);
bool mp_grid_append(struct mp_grid *grid, int tile, const char *path,
                    double fixed_start);
bool mp_grid_drop_files(struct mp_grid *grid, int tile, int num_files,
                        const char **paths);
bool mp_grid_remove(struct mp_grid *grid, int tile, int index);
bool mp_grid_seek(struct mp_grid *grid, int tile, double value, bool absolute);
bool mp_grid_get_progress(struct mp_grid *grid, int tile, double *position,
                          double *duration, int *rows, int *columns);
bool mp_grid_set_paused(struct mp_grid *grid, int tile, bool paused);
bool mp_grid_set_mute(struct mp_grid *grid, int tile, bool muted);
bool mp_grid_toggle_solo(struct mp_grid *grid, int tile);
bool mp_grid_set_volume(struct mp_grid *grid, int tile, double volume);
bool mp_grid_set_speed(struct mp_grid *grid, int tile, double speed);
bool mp_grid_set_fixed_start(struct mp_grid *grid, int tile, double position);
bool mp_grid_set_zoom(struct mp_grid *grid, int tile, bool active,
                      double center_x, double center_y);
struct mp_rect mp_grid_calc_output_rect(int width, int height, int rows,
                                        int columns, int aspect_w,
                                        int aspect_h);
struct mp_rect mp_grid_output_rect(struct mp_grid *grid, int width, int height);
int mp_grid_hit_test(struct mp_grid *grid, int x, int y, int width, int height);
bool mp_grid_zoom_at(struct mp_grid *grid, int x, int y, int width, int height,
                     bool active, bool move_only);

bool mp_grid_load_project(struct mp_grid *grid, const char *path);
bool mp_grid_save_project(struct mp_grid *grid, const char *path,
                          bool update_fixed_positions);
bool mp_grid_create_desktop_shortcut(struct mp_grid *grid,
                                     char **shortcut_path);
bool mp_grid_resume_project(struct mp_grid *grid, double *main_position);
void mp_grid_update_resume(struct mp_grid *grid);
void mp_grid_to_node(struct mp_grid *grid, struct mpv_node *node);

// VO callback. This is deliberately platform-neutral; gpu-next and a future
// macvk backend consume the same snapshot contract.
bool mp_grid_vo_snapshot(void *ctx, struct mp_grid_snapshot *snapshot);
bool mp_grid_vo_empty(void *ctx);
bool mp_grid_vo_layout(void *ctx, int *rows, int *columns);
void mp_grid_request_redraw(struct mp_grid *grid);
void mp_grid_set_vo(struct mp_grid *grid, struct vo *vo);
void mp_grid_mix_audio(void *ctx, void **data, int samples, int rate,
                       int format, const struct mp_chmap *channels);
