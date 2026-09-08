#include "gv_input.h"

#include <string.h>

/* Sticks rest slightly off centre on worn hardware; this is generous enough to
 * stop drift without eating small deliberate inputs. */
#define GV_STICK_DEADZONE 0.22f
#define GV_TRIGGER_THRESHOLD 0.35f
#define GV_AXIS_MAX 32767.0f

void gv_input_init(gv_input_state *in)
{
    if (!in) return;
    memset(in, 0, sizeof(*in));
    in->aim_dir = gv_v2_make(1.0f, 0.0f);
    in->viewport.w = GV_VIEW_W;
    in->viewport.h = GV_VIEW_H;
    in->pad_id = -1;

    /* Adopt whichever controller is already plugged in. */
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        in->pad = SDL_GameControllerOpen(i);
        if (in->pad) {
            SDL_Joystick *j = SDL_GameControllerGetJoystick(in->pad);
            in->pad_id = j ? SDL_JoystickInstanceID(j) : -1;
            break;
        }
    }
}

void gv_input_shutdown(gv_input_state *in)
{
    if (!in) return;
    if (in->pad) {
        SDL_GameControllerClose(in->pad);
        in->pad = NULL;
    }
    in->pad_id = -1;
}

bool gv_input_has_pad(const gv_input_state *in)
{
    return in && in->pad != NULL;
}

void gv_input_begin_frame(gv_input_state *in)
{
    if (!in) return;
    /* The wheel accumulates within a frame and must be zeroed here.
     *
     * The navigation queue deliberately is NOT cleared: it is drained by its
     * consumer (gv_input_next_nav) every frame, so resetting it here would only
     * ever throw away actions queued between frames — which is exactly what an
     * event injected through gv_app_handle_event outside the frame loop is.
     * Clearing it made the public "feed me an event" entry point silently
     * useless. */
    in->wheel = 0.0f;
    in->mouse_moved_recently = false;
}

void gv_input_set_viewport(gv_input_state *in, SDL_Rect viewport)
{
    if (!in) return;
    if (viewport.w > 0 && viewport.h > 0) in->viewport = viewport;
}

static void gv_push_nav(gv_input_state *in, gv_nav_action action)
{
    if (in->nav_count >= GV_NAV_QUEUE) return;
    in->nav[in->nav_count++] = action;
}

void gv_input_handle_event(gv_input_state *in, const SDL_Event *e)
{
    if (!in || !e) return;

    switch (e->type) {
    case SDL_MOUSEMOTION:
        in->mouse_window = gv_v2_make((float)e->motion.x, (float)e->motion.y);
        in->mouse_moved_recently = true;
        /* Touching the mouse takes aim back from the controller. */
        if (e->motion.xrel != 0 || e->motion.yrel != 0) in->using_pad = false;
        break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        bool down = (e->type == SDL_MOUSEBUTTONDOWN);
        if (e->button.button == SDL_BUTTON_LEFT) in->mouse_left = down;
        if (e->button.button == SDL_BUTTON_RIGHT) in->mouse_right = down;
        in->mouse_window = gv_v2_make((float)e->button.x, (float)e->button.y);
        if (down && e->button.button == SDL_BUTTON_LEFT) gv_push_nav(in, GV_NAV_CONFIRM);
        break;
    }

    case SDL_MOUSEWHEEL: {
        float amount = (float)e->wheel.y;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) amount = -amount;
        in->wheel += amount;
        break;
    }

    case SDL_KEYDOWN:
        if (e->key.repeat) {
            /* Repeats drive menu scrolling but never gameplay. */
            switch (e->key.keysym.scancode) {
            case SDL_SCANCODE_UP: case SDL_SCANCODE_W: gv_push_nav(in, GV_NAV_UP); break;
            case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: gv_push_nav(in, GV_NAV_DOWN); break;
            case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: gv_push_nav(in, GV_NAV_LEFT); break;
            case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: gv_push_nav(in, GV_NAV_RIGHT); break;
            default: break;
            }
            break;
        }
        switch (e->key.keysym.scancode) {
        case SDL_SCANCODE_UP: case SDL_SCANCODE_W: gv_push_nav(in, GV_NAV_UP); break;
        case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: gv_push_nav(in, GV_NAV_DOWN); break;
        case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: gv_push_nav(in, GV_NAV_LEFT); break;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: gv_push_nav(in, GV_NAV_RIGHT); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_SPACE:
            gv_push_nav(in, GV_NAV_CONFIRM);
            break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE:
            gv_push_nav(in, GV_NAV_BACK);
            break;
        default: break;
        }
        break;

    case SDL_CONTROLLERDEVICEADDED:
        if (!in->pad) {
            in->pad = SDL_GameControllerOpen(e->cdevice.which);
            if (in->pad) {
                SDL_Joystick *j = SDL_GameControllerGetJoystick(in->pad);
                in->pad_id = j ? SDL_JoystickInstanceID(j) : -1;
            }
        }
        break;

    case SDL_CONTROLLERDEVICEREMOVED:
        if (in->pad && e->cdevice.which == in->pad_id) {
            SDL_GameControllerClose(in->pad);
            in->pad = NULL;
            in->pad_id = -1;
            in->using_pad = false;
            /* Try to adopt another connected controller. */
            for (int i = 0; i < SDL_NumJoysticks(); ++i) {
                if (!SDL_IsGameController(i)) continue;
                in->pad = SDL_GameControllerOpen(i);
                if (in->pad) {
                    SDL_Joystick *j = SDL_GameControllerGetJoystick(in->pad);
                    in->pad_id = j ? SDL_JoystickInstanceID(j) : -1;
                    break;
                }
            }
        }
        break;

    case SDL_CONTROLLERBUTTONDOWN:
        in->using_pad = true;
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: gv_push_nav(in, GV_NAV_UP); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: gv_push_nav(in, GV_NAV_DOWN); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: gv_push_nav(in, GV_NAV_LEFT); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: gv_push_nav(in, GV_NAV_RIGHT); break;
        case SDL_CONTROLLER_BUTTON_A: case SDL_CONTROLLER_BUTTON_START:
            gv_push_nav(in, GV_NAV_CONFIRM);
            break;
        case SDL_CONTROLLER_BUTTON_B: gv_push_nav(in, GV_NAV_BACK); break;
        default: break;
        }
        break;

    default:
        break;
    }
}

gv_nav_action gv_input_next_nav(gv_input_state *in)
{
    if (!in || in->nav_count <= 0) return GV_NAV_NONE;
    gv_nav_action a = in->nav[0];
    for (int i = 1; i < in->nav_count; ++i) in->nav[i - 1] = in->nav[i];
    in->nav_count--;
    return a;
}

/* --------------------------------------------------------------- mapping */

static float gv_axis(SDL_GameController *pad, SDL_GameControllerAxis axis)
{
    if (!pad) return 0.0f;
    return (float)SDL_GameControllerGetAxis(pad, axis) / GV_AXIS_MAX;
}

/* Radial deadzone with rescaling, so the usable range still reaches 1.0. */
static gv_v2 gv_stick(SDL_GameController *pad, SDL_GameControllerAxis ax,
                      SDL_GameControllerAxis ay)
{
    gv_v2 v = gv_v2_make(gv_axis(pad, ax), gv_axis(pad, ay));
    float len = gv_v2_len(v);
    if (len < GV_STICK_DEADZONE) return gv_v2_zero();
    float scaled = (len - GV_STICK_DEADZONE) / (1.0f - GV_STICK_DEADZONE);
    return gv_v2_mul(gv_v2_mul(v, 1.0f / len), gv_clampf(scaled, 0.0f, 1.0f));
}

gv_input gv_input_build(gv_input_state *in, gv_gfx *gfx, const gv_world *w)
{
    gv_input out;
    memset(&out, 0, sizeof(out));
    if (!in || !gfx || !w) return out;

    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    const gv_player *p = &w->player;

    /* ---- thrust ---- */
    gv_v2 move = gv_v2_zero();
    if (keys) {
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) move.y -= 1.0f;
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) move.y += 1.0f;
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) move.x -= 1.0f;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) move.x += 1.0f;
    }
    gv_v2 pad_move = gv_stick(in->pad, SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY);
    if (gv_v2_len_sq(pad_move) > 0.0f) {
        move = pad_move;
        in->using_pad = true;
    }
    out.move = gv_v2_clamp_len(move, 1.0f);

    /* ---- aim ----
     * The right stick wins while it is being pushed; otherwise the mouse
     * position decides. Either way the last direction persists, so the ship
     * never snaps to an arbitrary heading when input goes idle. */
    gv_v2 pad_aim = gv_stick(in->pad, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY);
    if (gv_v2_len_sq(pad_aim) > 0.0f) {
        in->aim_dir = gv_v2_norm_or(pad_aim, in->aim_dir);
        in->using_pad = true;
        out.aim_point = gv_v2_madd(p->pos, in->aim_dir, 320.0f);
    } else if (in->using_pad) {
        out.aim_point = gv_v2_madd(p->pos, in->aim_dir, 320.0f);
    } else {
        /* Map window pixels through the letterboxed viewport into view space,
         * then into the world. */
        float vx = (float)in->viewport.w > 0.0f
                       ? (in->mouse_window.x - (float)in->viewport.x) *
                             (float)GV_VIEW_W / (float)in->viewport.w
                       : in->mouse_window.x;
        float vy = (float)in->viewport.h > 0.0f
                       ? (in->mouse_window.y - (float)in->viewport.y) *
                             (float)GV_VIEW_H / (float)in->viewport.h
                       : in->mouse_window.y;
        gv_v2 world = gv_gfx_view_to_world(gfx, gv_v2_make(vx, vy));
        out.aim_point = world;
        in->aim_dir = gv_v2_norm_or(gv_v2_sub(world, p->pos), in->aim_dir);
    }

    /* ---- tether: left mouse, right trigger, or right shift ---- */
    bool tether = in->mouse_left;
    if (gv_axis(in->pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > GV_TRIGGER_THRESHOLD) tether = true;
    if (in->pad && SDL_GameControllerGetButton(in->pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) {
        tether = true;
    }
    out.tether_held = tether;
    out.tether_pressed = tether && !in->tether_prev;
    in->tether_prev = tether;

    /* ---- pulse: right mouse, space, or A ---- */
    bool pulse = in->mouse_right;
    if (keys && keys[SDL_SCANCODE_SPACE]) pulse = true;
    if (in->pad && SDL_GameControllerGetButton(in->pad, SDL_CONTROLLER_BUTTON_A)) pulse = true;
    out.pulse_pressed = pulse && !in->pulse_prev;
    in->pulse_prev = pulse;

    /* ---- focus: either shift, or the left trigger ---- */
    bool focus = keys && (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]);
    if (gv_axis(in->pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > GV_TRIGGER_THRESHOLD) focus = true;
    out.focus_held = focus;

    /* ---- reel: wheel, Q/E, or the shoulder buttons ---- */
    float reel = gv_clampf(in->wheel, -1.0f, 1.0f);
    if (keys) {
        if (keys[SDL_SCANCODE_E]) reel += 1.0f;
        if (keys[SDL_SCANCODE_Q]) reel -= 1.0f;
    }
    if (in->pad) {
        if (SDL_GameControllerGetButton(in->pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) reel += 1.0f;
        if (SDL_GameControllerGetButton(in->pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) reel -= 1.0f;
    }
    out.reel = gv_clampf(reel, -1.0f, 1.0f);

    return out;
}
