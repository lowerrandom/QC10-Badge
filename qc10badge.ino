// Queercon 10 Badge Prototype
/*****************************
 * This code is intended to be run on a JeeNode-compatible platform.
 *
 * <http://www.queercon.org>
 */
// MIT License. http://opensource.org/licenses/mit-license.php
//
// Based in part on example code provided by:
// (c) 17-May-2010 <jc@wippler.nl>
// (c) 11-Jan-2011 <jc@wippler.nl>
//
// Otherwise:
// (c) 13-Dec-2012 George Louthan <georgerlouth@nthefourth.com>
#include <JeeLib.h>
#include <avr/eeprom.h>
#include <avr/pgmspace.h>

#define CONFIG_STRUCT_VERSION 156
#define BADGES_IN_SYSTEM 120

#define USE_LEDS 1

//EEPROM the learning state?
#define LEARNING 1 // Whether to auto-negotiate my ID.
#define DEFAULT_CROSSFADE_STEP 2

extern "C"
{
  #include "animations.h"
  #include "tlc5940.h"
  #include "main.h"
}

// NB: It's nice if the listen duration is divisible by BADGES_IN_SYSTEM 
#define R_SLEEP_DURATION 5000
#define R_LISTEN_DURATION 5000
#define R_LISTEN_WAKE_PAD 1000
#define R_NUM_SLEEP_CYCLES 6

#define UBER_COUNT 10

// Running timer results for our busy-waiting loop.
unsigned long last_time;
unsigned long current_time;
unsigned long elapsed_time;

#define PREBOOT_INTERVAL 20000
#define PREBOOT_SHOW_COUNT_AT 2000
#define PREBOOT_SHOW_UBERCOUNT_AT 12500

#define BADGE_METER_INTERVAL 6

// Configuration settings struct, to be stored on the EEPROM.
struct {
    uint8_t check;
    uint8_t freq, rcv_group, rcv_id, bcn_group, bcn_id;
    uint16_t badge_id;
    uint16_t badges_in_system;
    uint16_t r_sleep_duration, r_listen_duration, r_listen_wake_pad,
             r_num_sleep_cycles;
} config;

// Payload struct
struct qcbpayload {
  uint16_t from_id, authority, cycle_number, timestamp, badges_in_system;
  uint8_t ver;
} in_payload, out_payload;

byte badges_seen[BADGES_IN_SYSTEM];
uint8_t total_badges_seen = 0;
byte neighbor_badges[BADGES_IN_SYSTEM] = {0}; // All zeros.

// Convert our configuration frequency code to the actual frequency.
static word code2freq(byte code) {
    return code == 4 ? 433 : code == 9 ? 915 : 868;
}

// Convert our configuration frequency code to RF12's frequency const.
static word code2type(byte code) {
    return code == 4 ? RF12_433MHZ : code == 9 ? RF12_915MHZ : RF12_868MHZ;
}

// Print our current configuration to the Serial console.
static void showConfig() {
#if !(USE_LEDS)
    Serial.print("I am badge ");
    Serial.println(config.badge_id);
    Serial.print(' ');
    Serial.print(code2freq(config.freq));
    Serial.print(':');
    Serial.print((int) config.rcv_group);
    Serial.print(':');
    Serial.print((int) config.rcv_id);
    Serial.print(" -> ");
    Serial.print(code2freq(config.freq));
    Serial.print(':');
    Serial.print((int) config.bcn_group);
    Serial.print(':');
    Serial.print((int) config.bcn_id);
    Serial.println();
#endif
}

// Load our configuration from EEPROM (also computing some payload).
static void loadConfig() {
    byte* p = (byte*) &config;
    for (byte i = 0; i < sizeof config; ++i)
        p[i] = eeprom_read_byte((byte*) i);
    // if loaded config is not valid, replace it with defaults
    if (config.check != CONFIG_STRUCT_VERSION) {
        config.check = CONFIG_STRUCT_VERSION;
        config.freq = 4;
        config.rcv_group = 0;
        config.rcv_id = 1;
        config.bcn_group = 0;
        config.bcn_id = 1;
        config.badge_id = 1;
        config.badges_in_system = BADGES_IN_SYSTEM;
        config.r_sleep_duration = R_SLEEP_DURATION;
        config.r_listen_duration = R_LISTEN_DURATION;
        config.r_listen_wake_pad = R_LISTEN_WAKE_PAD;
        config.r_num_sleep_cycles = R_NUM_SLEEP_CYCLES;
        saveConfig();
        
        memset(badges_seen, 0, BADGES_IN_SYSTEM);
        total_badges_seen = 0;
        uint8_t badge_id;
        for (badge_id=0; badge_id<BADGES_IN_SYSTEM; badge_id++) {
          total_badges_seen += (uint8_t)has_seen_badge(badge_id);
        }
    }
    
    // Store the parts of our config that rarely change in the outgoing payload.
    out_payload.from_id = config.badge_id;
    out_payload.badges_in_system = config.badges_in_system;
    out_payload.ver = config.check;
    
    showConfig();
    rf12_initialize(config.rcv_id, code2type(config.freq), 1);
}

static boolean has_seen_badge(uint16_t badge_id) {
  return (badges_seen[badge_id] & 0b0000001);
}

static boolean can_see_badge(uint16_t badge_id) {
  return (neighbor_badges[badge_id] & 0b0000001);
}

// Returns true if this is the first time we've seen the badge.
static boolean just_saw_badge(uint16_t badge_id) {
  neighbor_badges[badge_id] |= 0b00000001;
  boolean seen_before = has_seen_badge(badge_id);
  badges_seen[badge_id] |= 0b00000001;
  if (seen_before) {
    return false;
  }
  total_badges_seen++;
  // TODO: save badges_seen to EEPROM.
  return true;
}

// Save our current configuration to the EEPROM. Also calls loadConfig().
// In general, this should be called after any configuration change because it
// does a little housekeeping.
static void saveConfig() {
    byte* p = (byte*) &config;
    for (byte i = 0; i < sizeof config; ++i)
        eeprom_write_byte((byte*) i, p[i]);
    loadConfig();
}

#define LOOP_TRUE 1
#define LOOP_FALSE 0

#define UBERFADE_TRUE 1
#define UBERFADE_FALSE 0

static void saveBadge(uint16_t badge_id) {
  int badge_address = (sizeof config) + badge_id;
  eeprom_write_byte((uint8_t *) badge_id, badges_seen[badge_id]);
}
#define CROSSFADING 1
void setup () {
#if !(USE_LEDS)
    Serial.begin(57600);
    Serial.println(57600);
#endif
    loadConfig();
    last_time = millis();
    current_time = millis();
#if USE_LEDS
    startTLC();
    set_system_lights_animation(SYSTEM_PREBOOT_INDEX, LOOP_TRUE, DEFAULT_CROSSFADE_STEP);
    if (1) { // uber
      set_ring_lights_animation(SUPERUBER_INDEX, LOOP_FALSE, CROSSFADING, DEFAULT_CROSSFADE_STEP, 0, 0);
    }
#endif
}

uint16_t time_new_animation_allowed = 0;
uint16_t time_since_last_bling = 0;
uint16_t seconds_between_blings = 10;
uint8_t current_bling = 0;
uint8_t num_blings = 10;
uint8_t shown_badgecount = 0;
uint8_t shown_ubercount = 0;
uint8_t in_preboot = 1;
uint8_t current_sys = 0;
uint8_t current_uber = 4;
unsigned long led_next_ring = 0;
unsigned long led_next_uber_fade = 0;
unsigned long led_next_sys = 0;
uint8_t idling = 0;

void show_badge_count() {
  // 13 is all. 24 is none.
  uint8_t end_index = 26 - (total_badges_seen-1 / BADGE_METER_INTERVAL);
  if (end_index < 13)
    end_index = 13;

  set_ring_lights_animation(BADGECOUNT_INDEX, LOOP_FALSE, CROSSFADING, DEFAULT_CROSSFADE_STEP,
                              end_index, 0);
  led_next_ring = 0;
  time_new_animation_allowed = current_time + 5000;
}

void show_uber_count() {
  set_ring_lights_animation(UBERCOUNT_INDEX, LOOP_FALSE, CROSSFADING, DEFAULT_CROSSFADE_STEP,
                              0, 0);
  led_next_ring = 0;
  time_new_animation_allowed = current_time + 5000;
}

void loop () {
  
  // Compute t using elapsed time since last iteration of this loop.
  current_time = millis();
  elapsed_time = current_time - last_time;
  
#if USE_LEDS
  if (current_time < PREBOOT_INTERVAL) {
    if (current_time > PREBOOT_SHOW_COUNT_AT && !shown_badgecount) {
      show_badge_count();
      shown_badgecount = 1;
    }
    if (current_time > PREBOOT_SHOW_UBERCOUNT_AT && !shown_ubercount) {
      show_uber_count();
      shown_ubercount = 1;
    }
  } else if (in_preboot) {
    in_preboot = 0;
    // TODO: pick this better.
    led_next_sys = 0;
    time_since_last_bling = 0;
  }
  
  if (!in_preboot) {
    time_since_last_bling += elapsed_time;
    if (time_since_last_bling > seconds_between_blings * 1000) {
//        set_ring_lights_animation(UBLING_START_INDEX + current_bling, LOOP_FALSE, CROSSFADING, 
//                                  DEFAULT_CROSSFADE_STEP, 0, 0);//(config.badge_id<=10)); // TODO
        set_ring_lights_animation(NEWBADGE_INDEX, LOOP_FALSE, 0, 
                          DEFAULT_CROSSFADE_STEP, 0, 0);//(config.badge_id<=10)); // TODO
        current_bling = (current_bling + 1) % (UBLING_COUNT);
        time_since_last_bling = 0;
        set_system_lights_animation(current_sys, LOOP_TRUE, 0);
        current_sys = (current_sys + 1) % 7;
        idling = 0;
    }
    if (!led_ring_animating && !idling) {
      set_ring_lights_animation(UBER_START_INDEX + current_uber, LOOP_TRUE, CROSSFADING, 
                                    DEFAULT_CROSSFADE_STEP, 0, 0);
      idling = 1;
    }
  }
  
  
  if (current_time >= led_next_ring) {
    led_next_ring = ring_lights_update_loop() + current_time;
  }
  // TODO: IF UBER
  if (current_time >= led_next_uber_fade) {
    led_next_uber_fade = uber_ring_fade() + current_time;
  }
  if (current_time >= led_next_sys) {
    led_next_sys = system_lights_update_loop() + current_time;
  }
  
  if (in_preboot) return; // Don't look for other badges in preboot.

  
  
#endif
  last_time = current_time;
  
  //////// RADIO SECTION /////////
  
  // millisecond clock in the current sleep cycle:
  static uint16_t t = 0;
  // number of the current sleep cycle:
  static uint16_t cycle_number = 0;
  // whether we've successfully beaconed this cycle:
  static boolean sent_this_cycle = false;
  // at what t should we start trying to beacon:
  static uint16_t t_to_send = config.r_sleep_duration + (config.r_listen_wake_pad / 2) + 
    ((config.r_listen_duration - config.r_listen_wake_pad) / config.badges_in_system) * config.badge_id;
  // my "authority", lower is more authoritative
  static uint16_t my_authority = config.badge_id;
  // lowest badge id I've seen this cycle, used to calculate my next initial authority
  // (my authority is the lowest badge to whom I can directly communicate; if one
  //  of my neighbors is communicating directly with a lower id badge than I am,
  //  that neighbor will be more authoritative than me.):
  static uint16_t lowest_badge_this_cycle = config.badge_id;
  // whether the radio is asleep:
  static boolean badge_is_sleeping = false;
  t += elapsed_time;
  

  // Radio duty cycle.
  if (cycle_number != config.r_num_sleep_cycles && t < config.r_sleep_duration) {
    // Radio sleeps unless we're in the last sleep cycle of an interval
    if (!badge_is_sleeping) {
      // Go to sleep if necessary, printing cycle information.
      rf12_sleep(0);
#if !(USE_LEDS)
      Serial.print("--|Cycle ");
      Serial.print(cycle_number);
      Serial.print("/");
      Serial.print(config.r_num_sleep_cycles);
      Serial.print(" t:");
      Serial.println(t);
      Serial.println("--|Sleeping radio.");
#endif
      badge_is_sleeping = true;
    }
  }
  else if (t < config.r_sleep_duration + config.r_listen_duration) {
    // This is the part of the sleep cycle during which we should be listening
    if (badge_is_sleeping) {
      // Wake up if necessary, printing cycle information.
      rf12_sleep(-1);
#if !(USE_LEDS)
      Serial.print("--|Cycle ");
      Serial.print(cycle_number);
      Serial.print("/");
      Serial.print(config.r_num_sleep_cycles);
      Serial.print(" t:");
      Serial.println(t);
      Serial.println("--|Waking radio.");
#endif
      badge_is_sleeping = false;
    }
    // Radio listens
    
    if (rf12_recvDone() && rf12_crc == 0) {
        // We've received something. Is it valid?
        if (rf12_len == sizeof in_payload) {      
            // TODO: confirm correct version.      
#if !(USE_LEDS)
            // Print the metadata and badge ID of the beacon we've received.
            Serial.print("<-|RCV OK ");
            Serial.print(rf12_grp);
            Serial.print("g ");
            Serial.print(rf12_hdr);
            Serial.print("hdr; beacon from ");
#endif
            in_payload = *(qcbpayload *)rf12_data;
            // Increment our beacon count in the current position in our
            // sliding window.
            //received_numbers[receive_cycle]+=1;
            // If we're in ID learning mode, and we've heard an ID that we
            // thought was supposed to be us:
            if (LEARNING && in_payload.from_id == config.badge_id) {
                // Increment our ID by one.
                config.badge_id += 1;
                saveConfig();
#if !(USE_LEDS)
                Serial.print("--|Duplicate ID detected. Incrementing mine to ");
                Serial.println(config.badge_id);
#endif
                t_to_send = config.r_sleep_duration + (config.r_listen_wake_pad / 2) + 
                            ((config.r_listen_duration - config.r_listen_wake_pad) / config.badges_in_system) * config.badge_id;
                // TODO: don't pick an ID that we've heard recently/before.
            }

            lowest_badge_this_cycle = min(in_payload.from_id, lowest_badge_this_cycle);
            if (in_payload.authority < my_authority) {
#if !(USE_LEDS)
              Serial.print("|--Detected a higher authority ");
              Serial.print(in_payload.authority);
              Serial.print(" than my current ");
              Serial.println(my_authority);
              Serial.print("Alter clock cycle by ");
              Serial.print((int)in_payload.cycle_number - (int)cycle_number);
              Serial.print(" and t by ");
              Serial.println((int)in_payload.timestamp - (int)t);
#endif
              my_authority = in_payload.authority;
              cycle_number = in_payload.cycle_number;
              if (in_payload.timestamp < t_to_send) {
                sent_this_cycle = false;
              }
              t = in_payload.timestamp;
            }
          } else {
            // Wrong length.
#if !(USE_LEDS)
            Serial.println("Malformed packet received.");
#endif
        }
    }
    if (!sent_this_cycle && t > t_to_send) {
      // Beacon.
#if !(USE_LEDS)
      Serial.println("--|BCN required.");
#endif
      out_payload.authority = my_authority;
      out_payload.cycle_number = cycle_number;
      out_payload.timestamp = t;
      if (rf12_canSend()) {
        sent_this_cycle = true;
        // Beacon.
#if !(USE_LEDS)
        Serial.print("->|BCN my number ");
#endif
        rf12_sendStart(0, &out_payload, sizeof out_payload);
      }
    }
  }
  else {
    // Time for a new sleep cycle
    t = 0;
    cycle_number++;
    sent_this_cycle = false;
    if (cycle_number > config.r_num_sleep_cycles) {
      // Time for a new interval
      my_authority = lowest_badge_this_cycle;
      lowest_badge_this_cycle = config.badge_id;
      cycle_number = 0;
    }
  }
}
