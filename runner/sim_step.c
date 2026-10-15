/* sim_step.c -- one simulation tick from sealed inputs (see sim_step.h). */
#include "sim_step.h"

#include <string.h>

#include "glue.h"
#include "audio/mixer.h"
#include "video/genesis_bus.h"
#include "video/genesis_machine.h"

/* One wall frame of master cycles: the audio chips (which run at master/240)
 * advance this far every tick whatever the 68K did (see main.c's comment
 * history on why the drain is the wall frame, not g_audio_cycle_counter).
 * Scaled to the console's frame geometry (NTSC 262 / PAL 313 lines) so the
 * per-frame sample count tracks the frame DURATION. The chips' sample clock is
 * fixed at master/240, so a PAL frame needs 313/262× the samples of an NTSC
 * one; leaving this at the NTSC constant drains every PAL frame ~16% short,
 * starving the DRC bridge's ring (audible underrun plus a ~40 ms audio_flush
 * stall each frame). The NTSC value is the historical 895780, so a 262-line
 * spec stays bit-exact unchanged. */
static uint32_t sim_wall_frame_master_cycles(void)
{
    return (uint32_t)((uint64_t)895780u * (uint64_t)g_console_lines_per_frame / 262u);
}

/* glue.c frame bookkeeping (not in glue.h; main.c historically declared
 * them at the call site). */
extern void glue_reset_frame_sync(void);
extern void glue_run_game_frame(void);
extern void glue_service_vblank(void);

static GenesisSimInput s_in;
static uint32_t        s_ticks;

/* Drain target when the caller supplies none: the chips must still advance
 * (a NULL mixer output skips ym2612_render/psg_render, i.e. the chip clock). */
#define SIM_SCRATCH_FM_FRAMES  4096
#define SIM_SCRATCH_PSG_FRAMES 16384
static int16_t s_scratch_fm[SIM_SCRATCH_FM_FRAMES * 2];
static int16_t s_scratch_psg[SIM_SCRATCH_PSG_FRAMES];

uint16_t genesis_sim_pad(int player)
{
    return (player >= 0 && player < GENESIS_SIM_MAX_PLAYERS) ? s_in.pad[player] : 0u;
}

uint32_t genesis_sim_human_mask(void) { return s_in.human_mask; }
const GenesisSimInput *genesis_sim_input(void) { return &s_in; }
void genesis_sim_input_restore(const GenesisSimInput *in) { if (in) s_in = *in; }
uint32_t genesis_sim_tick_count(void) { return s_ticks; }
void genesis_sim_set_tick_count(uint32_t n) { s_ticks = n; }

int genesis_sim_step(const GenesisSimInput *in, const GenesisSimAudio *audio,
                     const GenesisSimHooks *hooks)
{
    static const GenesisSimHooks no_hooks;
    if (!hooks) hooks = &no_hooks;
    if (in) s_in = *in;

    glue_reset_frame_sync();
    glue_run_game_frame();
    for (int port = 0; port < 2; port++) {
        machine_set_pad_type(port, s_in.pad_type[port]);
        machine_set_pad(port, s_in.pad[port]);
    }
    if (hooks->pre_raster) hooks->pre_raster(hooks->ctx);
    glue_sched_frame_begin();        /* scheduler rule: busy-wait streaks restart */
    machine_run_frame(hooks->sink, hooks->sink_user);

    {
        GenesisSimAudio scratch = { s_scratch_fm, SIM_SCRATCH_FM_FRAMES, NULL,
                                    s_scratch_psg, SIM_SCRATCH_PSG_FRAMES, NULL };
        const GenesisSimAudio *a = (audio && audio->fm && audio->psg) ? audio : &scratch;
        size_t fm_n = 0, psg_n = 0;
        audio_mixer_drain(sim_wall_frame_master_cycles(),
                          a->fm,  a->fm_cap,  &fm_n,
                          a->psg, a->psg_cap, &psg_n);
        if (a->fm_count)  *a->fm_count  = fm_n;
        if (a->psg_count) *a->psg_count = psg_n;
    }
    if (hooks->post_drain && !hooks->post_drain(hooks->ctx))
        return 0;

    glue_service_vblank();
    glue_end_of_wall_frame();
    s_ticks++;
    return 1;
}
