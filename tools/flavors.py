FID_SWITCH      = 1
FID_LIGHT_ONOFF = 2
FID_LIGHT_DIM   = 3
FID_TW_SMART    = 4
FID_TW_PWM      = 5
FID_PLUG        = 6

FLAVORS = {
    FID_SWITCH: {
        "name":  "Switch",
        "label": "Switch",
        "poll":  "continuous",
        "registers": {
            1: {"name": "state", "rw": False, "max": 1},
            2: {"name": "led",   "rw": True,  "max": 1},
        },
    },
    FID_LIGHT_ONOFF: {
        "name":  "Light",
        "label": "Light",
        "poll":  "periodic",
        "registers": {
            1: {"name": "on_off", "rw": True, "max": 1},
        },
    },
    FID_LIGHT_DIM: {
        "name":  "Dimmer",
        "label": "Dimmer",
        "poll":  None,
        "registers": {
            1: {"name": "on_off",     "rw": True, "max": 1},
            2: {"name": "brightness", "rw": True, "max": 1000},
        },
    },
    FID_TW_SMART: {
        "name":  "Tunable white (SPI)",
        "label": "Tunable light",
        "poll":  None,
        "registers": {},
    },
    FID_TW_PWM: {
        "name":  "Tunable white (PWM)",
        "label": "Tunable light",
        "poll":  None,
        "registers": {
            1: {"name": "on_off", "rw": True, "max": 1},
            2: {"name": "cold",   "rw": True, "max": 1000},
            3: {"name": "warm",   "rw": True, "max": 1000},
        },
    },
    FID_PLUG: {
        "name":  "Plug",
        "label": "Plug",
        "poll":  "periodic",
        "registers": {
            1: {"name": "on_off", "rw": True, "max": 1},
            2: {"name": "led",    "rw": True,  "max": 1},
        },
    },
}
