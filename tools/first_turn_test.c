/*
 * first_turn_test.c — headless verification harness for the
 * "cannot attack on your first turn" rule.
 *
 * This file #includes src/main.c (with the headless main() excluded via
 * WAIFU_FM_NO_HEADLESS_MAIN) so it has direct access to the game's static
 * state: g_b_phase, g_b_turns, g_b_battle_atk_owner, g_com_lp, g_you_lp, etc.
 *
 * It replays a command file (same format as --commands) frame-by-frame through
 * waifu_fm_step(), and after each frame logs the game state. At the end it
 * reports PASS/FAIL:
 *
 *   FAIL  if the player ever entered a battle (IB_PLAYER_BATTLE with
 *         g_b_battle_atk_owner == 0) while g_b_turns == 1  (first turn).
 *   PASS  otherwise.
 *
 * Build:
 *   cc -O2 -std=c99 -Wall -Wextra -DWAIFU_FM_NO_HEADLESS_MAIN \
 *      -DPLATFORM=5 -DBY16=1 -DHARDWARE_DIV=1 \
 *      -Isrc/engine -Isrc/generated -Isrc/record -Isrc/game \
 *      tools/first_turn_test.c src/engine/renderer3d.c src/engine/common.c \
 *      src/engine/bmp_writer.c src/record/zmbv_mkv.c -lm -lz -o first_turn_test
 *
 * Run:
 *   ./first_turn_test scripts/direct_attack_test.txt [frames]
 */

#define WAIFU_FM_NO_HEADLESS_MAIN
#include "../src/main.c"

#include <stdarg.h>

/* ---- minimal command-file parser (mirrors the headless/SDL loaders) ---- */

#define MAX_COMMAND_EVENTS 4096
typedef struct { int start; int duration; WaifuFmInput input; } TCmd;

static void t_button(WaifuFmInput *in, const char *tok)
{
    char buf[32]; int i = 0;
    while (*tok && i < (int)sizeof(buf)-1) {
        if (*tok != ',' && *tok != '|' && *tok != '+') buf[i++] = (char)toupper((unsigned char)*tok);
        tok++;
    }
    buf[i] = '\0';
    if      (!strcmp(buf,"UP"))    in->up = 1;
    else if (!strcmp(buf,"DOWN"))  in->down = 1;
    else if (!strcmp(buf,"LEFT"))  in->left = 1;
    else if (!strcmp(buf,"RIGHT")) in->right = 1;
    else if (!strcmp(buf,"A")||!strcmp(buf,"LCTRL")||!strcmp(buf,"CTRL")) in->a = 1;
    else if (!strcmp(buf,"B")||!strcmp(buf,"LALT")||!strcmp(buf,"ALT"))   in->b = 1;
    else if (!strcmp(buf,"START")||!strcmp(buf,"RUN")||!strcmp(buf,"SPACE")) in->start = 1;
}

static void t_button_list(WaifuFmInput *in, const char *tok)
{
    char part[32]; int n = 0; const char *p = tok;
    for (;;) {
        int delim = (*p == '\0' || *p == ',' || *p == '|' || *p == '+');
        if (delim) {
            if (n > 0) { part[n] = '\0'; t_button(in, part); n = 0; }
            if (*p == '\0') break;
        } else if (n < (int)sizeof(part)-1) {
            part[n++] = *p;
        }
        ++p;
    }
}

static int t_load(const char *path, TCmd *events, int max_events)
{
    FILE *fp = fopen(path, "r");
    char line[512]; int count = 0;
    if (!fp) { fprintf(stderr, "could not open command file: %s\n", path); return -1; }
    while (fgets(line, sizeof(line), fp)) {
        char *p = line, *tok; TCmd ev; memset(&ev, 0, sizeof(ev));
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        tok = strtok(p, " \t\r\n"); if (!tok) continue;
        ev.start = atoi(tok);
        tok = strtok(NULL, " \t\r\n"); if (!tok) continue;
        ev.duration = atoi(tok); if (ev.duration < 1) ev.duration = 1;
        while ((tok = strtok(NULL, " \t\r\n")) != NULL) t_button_list(&ev.input, tok);
        if (count < max_events) events[count++] = ev;
    }
    fclose(fp);
    return count;
}

static WaifuFmInput t_input_for(int frame, const TCmd *events, int count)
{
    WaifuFmInput in; int i; memset(&in, 0, sizeof(in));
    for (i = 0; i < count; ++i)
        if (frame >= events[i].start && frame < events[i].start + events[i].duration) {
            in.up |= events[i].input.up; in.down |= events[i].input.down;
            in.left |= events[i].input.left; in.right |= events[i].input.right;
            in.a |= events[i].input.a; in.b |= events[i].input.b;
            in.start |= events[i].input.start;
        }
    return in;
}

static const char *phase_name(WaifuBattlePhase p)
{
    switch (p) {
    case IB_OPENING:           return "OPENING";
    case IB_PLAYER_HAND:       return "PLAYER_HAND";
    case IB_PLAYER_TOP:        return "PLAYER_TOP";
    case IB_CARD_PREVIEW:      return "CARD_PREVIEW";
    case IB_PLAYER_PLACE:      return "PLAYER_PLACE";
    case IB_PLAYER_BATTLE:     return "PLAYER_BATTLE";
    case IB_PLAYER_RETURN_TOP: return "PLAYER_RETURN_TOP";
    case IB_TURN_TO_COM:       return "TURN_TO_COM";
    case IB_COM_SELECT:        return "COM_SELECT";
    case IB_COM_PLACE:         return "COM_PLACE";
    case IB_COM_BATTLE:        return "COM_BATTLE";
    case IB_COM_RETURN:        return "COM_RETURN";
    case IB_TURN_TO_PLAYER:    return "TURN_TO_PLAYER";
    case IB_PLAYER_DRAW:       return "PLAYER_DRAW";
    case IB_RESULT:            return "RESULT";
    case IB_TALLY:             return "TALLY";
    }
    return "?";
}

static const char *state_name(WaifuInteractiveState s)
{
    switch (s) {
    case WAIFU_I_TITLE:       return "TITLE";
    case WAIFU_I_MENU:        return "MENU";
    case WAIFU_I_STORY_STUB:  return "STORY";
    case WAIFU_I_BATTLE:      return "BATTLE";
    }
    return "?";
}

int main(int argc, char **argv)
{
    const char *cmd_path = (argc > 1) ? argv[1] : "scripts/direct_attack_test.txt";
    int frames = (argc > 2) ? atoi(argv[2]) : 540;
    TCmd events[MAX_COMMAND_EVENTS];
    int event_count, f;
    int first_turn_player_battle_frame = -1;   /* frame player battle started on turn 1 */
    int first_turn_com_lp_after = -1;          /* com_lp seen right after a turn-1 player battle */
    int turn2_player_battle_frame = -1;        /* frame player battle started on turn >=2 */
    int saw_player_top_turn1 = 0;

    event_count = t_load(cmd_path, events, MAX_COMMAND_EVENTS);
    if (event_count < 0) return 2;

    waifu_fm_init();
    waifu_fm_reset_interactive();

    printf("== first_turn_test: replaying '%s' for %d frames ==\n", cmd_path, frames);

    for (f = 0; f < frames; ++f) {
        WaifuFmInput in = t_input_for(f, events, event_count);
        WaifuBattlePhase pre_phase = g_b_phase;
        int pre_owner = g_b_battle_atk_owner;
        int pre_com_lp = g_com_lp;
        int pre_turns = g_b_turns;

        waifu_fm_step(&in);

        /* Detect the transition INTO a player battle (owner 0). */
        if (pre_phase != IB_PLAYER_BATTLE && g_b_phase == IB_PLAYER_BATTLE &&
            g_b_battle_atk_owner == 0) {
            if (pre_turns == 1) {
                if (first_turn_player_battle_frame < 0) {
                    first_turn_player_battle_frame = f;
                    first_turn_com_lp_after = g_com_lp;
                }
                printf("[f=%4d] *** PLAYER ATTACK STARTED ON FIRST TURN (g_b_turns=%d) "
                       "com_lp=%d -> %d ***\n", f, pre_turns, pre_com_lp, g_com_lp);
            } else {
                if (turn2_player_battle_frame < 0) {
                    turn2_player_battle_frame = f;
                    printf("[f=%4d] player attack started on turn %d (allowed) "
                           "com_lp=%d\n", f, pre_turns, g_com_lp);
                }
            }
        }

        if (g_i_state == WAIFU_I_BATTLE && g_b_phase == IB_PLAYER_TOP && g_b_turns == 1)
            saw_player_top_turn1 = 1;

        /* Log state changes of interest (compact). */
        if (g_i_state == WAIFU_I_BATTLE &&
            (g_b_phase != pre_phase || g_com_lp != pre_com_lp || g_b_turns != pre_turns)) {
            printf("[f=%4d] state=%-6s phase=%-17s turn=%d you_lp=%d com_lp=%d "
                   "atk_owner=%d\n",
                   f, state_name(g_i_state), phase_name(g_b_phase),
                   g_b_turns, g_you_lp, g_com_lp, g_b_battle_atk_owner);
        }
    }

    printf("== summary ==\n");
    printf("saw PLAYER_TOP on turn 1:              %s\n", saw_player_top_turn1 ? "yes" : "no");
    printf("player battle on turn 1 (FORBIDDEN):   %s\n",
           first_turn_player_battle_frame >= 0 ? "YES -> BUG" : "no");
    printf("player battle on turn >=2 (allowed):   %s\n",
           turn2_player_battle_frame >= 0 ? "yes" : "no (script did not reach it)");

    if (first_turn_player_battle_frame >= 0) {
        printf("\nRESULT: FAIL — player was able to attack on the first turn "
               "(battle began at frame %d, com_lp dropped to %d).\n",
               first_turn_player_battle_frame, first_turn_com_lp_after);
        return 1;
    }
    printf("\nRESULT: PASS — no player attack was possible on the first turn.\n");
    return 0;
}
