// Compiles the real, unmodified shotStopper.ino as C++ against the fakes.
//
// Its quoted includes ("AcaiaArduinoBLE.h", "OTASetup.h") resolve next to the
// sketch, exactly as in the Arduino IDE; its angle-bracket includes resolve to
// test/fakes/.
#include <Arduino.h>

#include "sketch_prototypes.h"

#include "../../shotStopper.ino"

#include "sketch_api.h"

// Sketch #defines, exported so tests compiled separately can use them.
namespace sketch {
const int kPinIn = IN;
const int kPinOut = OUT;
const int kPinReedIn = REED_IN;
const int kEepromSize = EEPROM_SIZE;
const int kSignatureAddr = SIGNATURE_ADDR;
const int kSignatureValue = SIGNATURE_VALUE;
const int kEnabledAddr = ENABLED_ADDR;
const int kWeightAddr = WEIGHT_ADDR;
const int kOffsetAddr = OFFSET_ADDR;
const int kMomentaryAddr = MOMENTARY_ADDR;
const int kReedSwitchAddr = REEDSWITCH_ADDR;
const int kAutoTareAddr = AUTOTARE_ADDR;
const int kMinShotDurationAddr = MIN_SHOT_DURATION_S_ADDR;
const int kMaxShotDurationAddr = MAX_SHOT_DURATION_S_ADDR;
const int kDripDelayAddr = DRIP_DELAY_S_ADDR;
const int kWifiSsidAddr = WIFI_SSID_ADDR;
const int kWifiPassAddr = WIFI_PASS_ADDR;
const int kMaxOffset = MAX_OFFSET;
const int kFirmwareVersion = FIRMWARE_VERSION;
}  // namespace sketch
