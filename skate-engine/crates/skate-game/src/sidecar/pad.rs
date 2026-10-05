//! TF2 usercmd -> virtual Xbox pad.
//!
//! Movement keys are the left stick. Mouse motion feeds a spring-loaded right
//! stick, so "pull the mouse down, then flick it up" is an ollie, matching
//! Skate's flick-it gestures. Buttons map to Source IN_* bits:
//!
//! | TF2 input            | IN_ bit     | Pad          |
//! |----------------------|-------------|--------------|
//! | jump                 | IN_JUMP     | A (push)     |
//! | duck                 | IN_DUCK     | X            |
//! | reload               | IN_RELOAD   | B            |
//! | +use                 | IN_USE      | Y (on/off)   |
//! | attack / attack2     | IN_ATTACK*  | LT / RT      |
//! | +speed / attack3     | IN_SPEED/ATTACK3 | LB / RB |
//! | +walk                | IN_WALK     | left stick click |
//!
//! With a game controller the client sets IN_BULLRUSH (unused by TF2) and
//! sends the raw right stick, up positive, in mousedx/mousedy (+-32767); the
//! stick then goes straight through, using Skate's own deadzone.
use skate_core::input::xbox::XboxState;

const IN_ATTACK: u32 = 1 << 0;
const IN_JUMP: u32 = 1 << 1;
const IN_DUCK: u32 = 1 << 2;
const IN_USE: u32 = 1 << 5;
const IN_ATTACK2: u32 = 1 << 11;
const IN_RELOAD: u32 = 1 << 13;
const IN_SPEED: u32 = 1 << 17;
const IN_WALK: u32 = 1 << 18;
const IN_BULLRUSH: u32 = 1 << 22;
const IN_ATTACK3: u32 = 1 << 25;

// XInput wButtons.
const PAD_LEFT_THUMB: u16 = 0x0040;
const PAD_LEFT_SHOULDER: u16 = 0x0100;
const PAD_RIGHT_SHOULDER: u16 = 0x0200;
const PAD_A: u16 = 0x1000;
const PAD_B: u16 = 0x2000;
const PAD_X: u16 = 0x4000;
const PAD_Y: u16 = 0x8000;

const BUTTONS: [(u32, u16); 7] = [
    (IN_JUMP, PAD_A),
    (IN_DUCK, PAD_X),
    (IN_RELOAD, PAD_B),
    (IN_USE, PAD_Y),
    (IN_SPEED, PAD_LEFT_SHOULDER),
    (IN_ATTACK3, PAD_RIGHT_SHOULDER),
    (IN_WALK, PAD_LEFT_THUMB),
];

pub(crate) struct Command {
    pub buttons: u32,
    /// -1..1, forward positive.
    pub forward: f32,
    /// -1..1, right positive.
    pub side: f32,
    /// Raw mouse counts since the previous usercmd (Source: +y is down).
    pub mouse: [f32; 2],
}

#[derive(Clone)]
pub(crate) struct VirtualPad {
    buttons: u32,
    left: [f32; 2],
    right: [f32; 2],
    pending_mouse: [f32; 2],
    /// Absolute right stick from a controller, when the client sends one.
    stick: Option<[f32; 2]>,
    gain: f32,
    decay: f32,
}

impl Default for VirtualPad {
    fn default() -> Self {
        let env = |name: &str, default: f32| {
            std::env::var(name).ok().and_then(|v| v.parse().ok()).unwrap_or(default)
        };
        Self {
            buttons: 0,
            left: [0.0; 2],
            right: [0.0; 2],
            pending_mouse: [0.0; 2],
            stick: None,
            gain: env("SKATE_MOUSE_GAIN", 0.02),
            decay: env("SKATE_MOUSE_DECAY", 0.7),
        }
    }
}

impl VirtualPad {
    /// Latch one usercmd. Mouse motion waits for the next native tick when
    /// this usercmd ran none, so short frames cannot drop a flick.
    pub fn accept(&mut self, command: &Command, _ticks: u32) {
        self.buttons = command.buttons;
        self.left = clamp_unit([command.side, command.forward]);
        if command.buttons & IN_BULLRUSH != 0 {
            self.stick = Some(clamp_unit(command.mouse.map(|v| v / 32767.0)));
            self.pending_mouse = [0.0; 2];
        } else {
            self.stick = None;
            self.pending_mouse[0] += command.mouse[0];
            self.pending_mouse[1] += command.mouse[1];
        }
    }

    /// The player's flick-stick feel (cl_skate_mouse_gain / _decay): stick
    /// deflection per mouse count, and how much of it remains each tick.
    pub fn set_mouse_response(&mut self, gain: f32, decay: f32) {
        if gain.is_finite() && gain > 0.0 {
            self.gain = gain.min(1.0);
        }
        if decay.is_finite() && (0.0..1.0).contains(&decay) {
            self.decay = decay;
        }
    }

    pub fn tick(&mut self) -> XboxState {
        self.right = if let Some(stick) = self.stick { stick } else { clamp_unit([
            self.right[0] * self.decay + self.pending_mouse[0] * self.gain,
            self.right[1] * self.decay - self.pending_mouse[1] * self.gain,
        ]) };
        self.pending_mouse = [0.0; 2];
        let mut buttons = 0;
        for (bit, pad) in BUTTONS {
            if self.buttons & bit != 0 {
                buttons |= pad;
            }
        }
        let trigger = |bit| if self.buttons & bit != 0 { 255 } else { 0 };
        XboxState {
            buttons,
            triggers: [trigger(IN_ATTACK), trigger(IN_ATTACK2)],
            left: self.left.map(axis),
            right: self.right.map(axis),
        }
    }
}

fn clamp_unit(v: [f32; 2]) -> [f32; 2] {
    let length = (v[0] * v[0] + v[1] * v[1]).sqrt();
    if length > 1.0 { [v[0] / length, v[1] / length] } else { v }
}

fn axis(v: f32) -> i16 {
    (v.clamp(-1.0, 1.0) * 32767.0).round() as i16
}

#[cfg(test)]
mod tests {
    use super::*;

    fn command(buttons: u32, mouse: [f32; 2]) -> Command {
        Command { buttons, forward: 1.0, side: 0.0, mouse }
    }

    #[test]
    fn mouse_flick_moves_right_stick_and_springs_back() {
        let mut pad = VirtualPad { gain: 0.02, decay: 0.7, ..VirtualPad::default() };
        pad.accept(&command(0, [0.0, 100.0]), 1);
        let down = pad.tick();
        assert_eq!(down.right[1], -32767, "mouse down is stick down");
        pad.accept(&command(0, [0.0, 0.0]), 1);
        for _ in 0..10 {
            pad.tick();
        }
        assert!(pad.tick().right[1].abs() < 1000);
    }

    #[test]
    fn controller_right_stick_passes_through() {
        let mut pad = VirtualPad::default();
        pad.accept(&command(IN_BULLRUSH, [0.0, -32767.0]), 1);
        assert_eq!(pad.tick().right, [0, -32767]);
        pad.accept(&command(IN_BULLRUSH, [16384.0, 0.0]), 1);
        assert_eq!(pad.tick().right, [16384, 0]);
    }

    #[test]
    fn jump_is_a_and_attack_is_left_trigger() {
        let mut pad = VirtualPad::default();
        pad.accept(&command(IN_JUMP | IN_ATTACK, [0.0; 2]), 1);
        let state = pad.tick();
        assert_eq!(state.buttons & PAD_A, PAD_A);
        assert_eq!(state.triggers, [255, 0]);
        assert_eq!(state.left, [0, 32767]);
    }
}
