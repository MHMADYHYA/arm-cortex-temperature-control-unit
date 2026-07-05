#include <string.h>
#include <stdio.h>
#include "i2cspm.h"
#include "sl_i2cspm.h"
#include "sl_i2cspm_instances.h"
#include "sl_sleeptimer.h"
#include "sl_simple_led.h"
#include "sl_simple_led_instances.h"
#include "sl_atomic.h"
#include "sl_segmentlcd.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_emu.h"
#include "em_chip.h"
#include "gpiointerrupt.h"
#include "sl_assert.h"
#include "sl_common.h"
#include "sl_interrupt_manager.h"
#include "em_device.h"
#include "em_timer.h"

// Temperature boundaries
#define TEMPERATURE_BAND_C               4

// SI7021_Config_Settings Si7021 Configuration Settings
#define SI7021_I2C_DEVICE                (sl_i2cspm_sensor) /**< I2C device used to control the Si7021  */
#define SI7021_I2C_BUS_ADDRESS           0x40               /**< I2C bus address                        */
#define SI7021_DEVICE_ID                 0x15               /**< Si7021 device ID                       */

// Si7021 command macro definitions
#define SI7021_CMD_MEASURE_RH            0xE5               /**< Measure Relative Humidity, Hold Master Mode */
#define SI7021_CMD_MEASURE_RH_NO_HOLD    0xF5               /**< Measure Relative Humidity, No Hold Master Mode */
#define SI7021_CMD_MEASURE_TEMP          0xE3               /**< Measure Temperature, Hold Master Mode */
#define SI7021_CMD_MEASURE_TEMP_NO_HOLD  0xF3               /**< Measure Temperature, No Hold Master Mode */
#define SI7021_CMD_READ_TEMP             0xE0               /**< Read Temperature Value from Previous RH Measurement */
#define SI7021_CMD_RESET                 0xFE               /**< Reset */
#define SI7021_CMD_WRITE_USER_REG1       0xE6               /**< Write RH/T User Register 1 */
#define SI7021_CMD_READ_USER_REG1        0xE7               /**< Read RH/T User Register 1 */
#define SI7021_CMD_WRITE_HEATER_CTRL     0x51               /**< Write Heater Control Register */
#define SI7021_CMD_READ_HEATER_CTRL      0x11               /**< Read Heater Control Register */
#define SI7021_CMD_READ_ID_BYTE1         { 0xFA, 0x0F }       /**< Read Electronic ID 1st Byte */
#define SI7021_CMD_READ_ID_BYTE2         { 0xFC, 0xC9 }       /**< Read Electronic ID 2nd Byte */
#define SI7021_CMD_READ_FW_REV           { 0x84, 0xB8 }       /**< Read Firmware Revision */

#define himom_T   25
#define keror_T   28
// Modes and motor states


// Global variables
static uint32_t relative_humidity;
static int32_t temperature;
static bool read_sensor_data = false;
static sl_sleeptimer_timer_handle_t delay_timer;

#define PWM_FREQ            1000   // 1kHz
#define INITIAL_DUTY_CYCLE  25     // 25%

static volatile float dutyCycle;

// Function prototypes
void pwmInit(void);
void setMotorSpeed(uint8_t duty);
void pwmInit(void) {
    // הפעלת השעונים הדרושים
    CMU_ClockEnable(cmuClock_GPIO, true);
    CMU_ClockEnable(cmuClock_TIMER0, true);

    // קביעת PD11 כיציאה דיגיטלית
    GPIO_PinModeSet(gpioPortC, 11, gpioModePushPull, 0);

    // הגדרות TIMER0
    TIMER_Init_TypeDef timerInit = TIMER_INIT_DEFAULT;
    TIMER_InitCC_TypeDef timerCCInit = TIMER_INITCC_DEFAULT;

    timerInit.enable = false;  // לא מתחילים את הטיימר עד שהכל מוגדר
    timerCCInit.mode = timerCCModePWM; // מצב PWM

    TIMER_Init(TIMER0, &timerInit);

    // ניתוב אות ה-PWM ליציאה PC11
    GPIO->TIMERROUTE[0].ROUTEEN  = GPIO_TIMER_ROUTEEN_CC0PEN;
    GPIO->TIMERROUTE[0].CC0ROUTE = (gpioPortC << _GPIO_TIMER_CC0ROUTE_PORT_SHIFT)
                                 | (11 << _GPIO_TIMER_CC0ROUTE_PIN_SHIFT);

    TIMER_InitCC(TIMER0, 0, &timerCCInit);

    // חישוב ערכי ה-PWM
    uint32_t timerFreq = CMU_ClockFreqGet(cmuClock_TIMER0);
    uint32_t topValue = timerFreq / PWM_FREQ;

    TIMER_TopSet(TIMER0, topValue);

    // אתחול ה-duty cycle ל-25%
    dutyCycle = INITIAL_DUTY_CYCLE;
    uint32_t dutyCount = (topValue * INITIAL_DUTY_CYCLE) / 100;
    TIMER_CompareSet(TIMER0, 0, dutyCount);

    // הפעלת הטיימר
    TIMER_Enable(TIMER0, true);
}

void setMotorSpeed(uint8_t duty) {
    if (duty > 100) {
        duty = 100;  // לוודא שהערך לא יעלה על 100%
    }
    dutyCycle = duty;  // שמירת הערך המעודכן

    // חישוב ה-duty cycle החדש
    uint32_t topValue = TIMER_TopGet(TIMER0);
    uint32_t dutyCount = (topValue * dutyCycle) / 100;

    // עדכון ערך ההשוואה של ה-TIMER
    TIMER_CompareSet(TIMER0, 0, dutyCount);
}

static I2C_TransferReturn_TypeDef SI7021_transaction(uint16_t flag, uint8_t *writeCmd, size_t writeLen, uint8_t *readCmd, size_t readLen) {
    I2C_TransferSeq_TypeDef seq;
    I2C_TransferReturn_TypeDef ret;
    seq.addr = SI7021_I2C_BUS_ADDRESS << 1;
    seq.flags = flag;

    switch (flag) {
        case I2C_FLAG_WRITE:
            seq.buf[0].data = writeCmd;
            seq.buf[0].len = writeLen;
            break;
        case I2C_FLAG_READ:
            seq.buf[0].data = readCmd;
            seq.buf[0].len = readLen;
            break;
        case I2C_FLAG_WRITE_READ:
            seq.buf[0].data = writeCmd;
            seq.buf[0].len = writeLen;
            seq.buf[1].data = readCmd;
            seq.buf[1].len = readLen;
            break;
        default:
            return i2cTransferUsageFault;
    }

    ret = I2CSPM_Transfer(SI7021_I2C_DEVICE, &seq);
    return ret;
}

uint32_t decode_rh(uint8_t* read_register) {
    uint32_t rhValue = ((uint32_t)read_register[0] << 8) + (read_register[1] & 0xfc);
    rhValue = (((rhValue) * 125) >> 16) - 6;
    return rhValue;
}

uint32_t decode_temp(uint8_t* read_register) {
    uint32_t tempValue = ((uint32_t)read_register[0] << 8) + (read_register[1] & 0xfc);
    float actual_temp = (((tempValue) * 175.72f) / 65536) - 46.85f;
    return (uint32_t)(actual_temp < 0 ? actual_temp - 0.5f : actual_temp + 0.5f);
}

static void SI7021_measure(uint32_t *rhData, int32_t *tData) {
    I2C_TransferReturn_TypeDef ret;
    uint8_t cmd;
    uint8_t readData[2];

    cmd = SI7021_CMD_MEASURE_RH_NO_HOLD;
    ret = SI7021_transaction(I2C_FLAG_WRITE, &cmd, 1, NULL, 0);
    EFM_ASSERT(ret == i2cTransferDone);

    uint32_t timeout = 500;
    while (timeout--) {
        ret = SI7021_transaction(I2C_FLAG_READ, NULL, 0, readData, 2);
        if (ret == i2cTransferDone) {
            break;
        }
    }
    EFM_ASSERT(timeout > 0);

    *rhData = decode_rh(readData);

    cmd = SI7021_CMD_READ_TEMP;
    ret = SI7021_transaction(I2C_FLAG_WRITE_READ, &cmd, 1, readData, 2);
    EFM_ASSERT(ret == i2cTransferDone);

    *tData = decode_temp(readData);
}

static void timer_callback(sl_sleeptimer_timer_handle_t *handle, void *data) {
    (void)handle;
    (void)data;
    sl_atomic_store(read_sensor_data, true);
}

static void initialise_timer() {
    uint32_t ticks = sl_sleeptimer_ms_to_tick(1000);
    sl_sleeptimer_start_periodic_timer(&delay_timer, ticks, timer_callback, NULL, 0, 0);
}


void gpioExternalPB(uint8_t pin) {
    if (pin == 13) {
        sl_led_turn_on(&sl_led_led0);
           sl_led_turn_off(&sl_led_led1);;
    }
}

void gpioBTN0(uint8_t pin) {
    if (pin == 1) {
        char temp_str[8];
        snprintf(temp_str, sizeof(temp_str), "MANUALR");
        sl_segment_lcd_write(temp_str);
        sl_led_turn_off(&sl_led_led1);
        sl_led_turn_on(&sl_led_led0);
        GPIO_PinOutSet(gpioPortD, 15);
        GPIO_PinOutClear(gpioPortD, 14);
        sl_sleeptimer_delay_millisecond(1000);
    }
}

void gpioBTN1(uint8_t pin) {
    if (pin == 6) {
        char temp_str[8];
        snprintf(temp_str, sizeof(temp_str), "MANUALL");
        sl_segment_lcd_write(temp_str);
        sl_led_turn_on(&sl_led_led1);
        sl_led_turn_off(&sl_led_led0);
        GPIO_PinOutClear(gpioPortD, 15);
        GPIO_PinOutSet(gpioPortD, 14);
        sl_sleeptimer_delay_millisecond(1000);
    }
}

void i2cspm_app_init(void) {
    pwmInit();
    I2C_TransferReturn_TypeDef ret;
    uint8_t cmdReadId2[2] = SI7021_CMD_READ_ID_BYTE2;
    uint8_t deviceId[8];

    ret = SI7021_transaction(I2C_FLAG_WRITE_READ, cmdReadId2, 2, deviceId, 8);
    EFM_ASSERT(ret == i2cTransferDone);
    EFM_ASSERT(deviceId[0] == SI7021_DEVICE_ID);

    initialise_timer();
    CMU_ClockEnable(cmuClock_GPIO, true);
    CMU_ClockEnable(cmuClock_LCD, true);
    sl_segment_lcd_init(false);

    GPIOINT_Init();
    GPIO_PinModeSet(gpioPortB, 1, gpioModeInputPull, 1);
    GPIOINT_CallbackRegister(1, gpioBTN0);
    GPIO_PinModeSet(gpioPortB, 6, gpioModeInputPull, 1);
    GPIOINT_CallbackRegister(6, gpioBTN1);
    GPIO_PinModeSet(gpioPortD, 13, gpioModeInputPull, 1);
    GPIOINT_CallbackRegister(13, gpioExternalPB);

    GPIO_ExtIntConfig(gpioPortB, 1, 1, false, true, true);
    GPIO_ExtIntConfig(gpioPortB, 6, 6, false, true, true);
    GPIO_ExtIntConfig(gpioPortD, 13, 13, false, true, true);

    GPIO_PinModeSet(gpioPortD, 15, gpioModePushPull, 1);
    GPIO_PinModeSet(gpioPortD, 14, gpioModePushPull, 1);

}

void i2cspm_app_process_action(void) {
    if (read_sensor_data) {
        read_sensor_data = false;

        SI7021_measure(&relative_humidity, &temperature);

        char temp_str[8];
        if (temperature <= himom_T) {
            snprintf(temp_str, sizeof(temp_str), "%ld COLD", temperature);
            sl_segment_lcd_write(temp_str);
            sl_led_turn_on(&sl_led_led0);
            sl_led_turn_off(&sl_led_led1);
            setMotorSpeed(100);
            GPIO_PinOutSet(gpioPortD, 15);
            GPIO_PinOutClear(gpioPortD, 14);
        } else if (temperature >= keror_T) {
            snprintf(temp_str, sizeof(temp_str), "%ld HOT", temperature);
            sl_segment_lcd_write(temp_str);
            sl_led_turn_off(&sl_led_led0);
            sl_led_turn_on(&sl_led_led1);
            setMotorSpeed(100);
            GPIO_PinOutClear(gpioPortD, 15);
            GPIO_PinOutSet(gpioPortD, 14);
        } else {
            snprintf(temp_str, sizeof(temp_str), "%ld COOL", temperature);
            sl_segment_lcd_write(temp_str);
            sl_led_turn_on(&sl_led_led0);
            sl_led_turn_off(&sl_led_led1);
            setMotorSpeed(10);
            GPIO_PinOutSet(gpioPortD, 15);
            GPIO_PinOutClear(gpioPortD, 14);

            }
        }
    }
