#ifndef APPOTECH_PRINTER_H
#define APPOTECH_PRINTER_H
#include <stddef.h>
#include <stdint.h>
#define APX_PRINTER_WIDTH 384u
enum apx_printer_mode { APX_PRINTER_GRAY,APX_PRINTER_DOT };
enum apx_printer_state { APX_PRINTER_OFF,APX_PRINTER_READY,APX_PRINTER_SAMPLING,
 APX_PRINTER_PRINTING,APX_PRINTER_DONE,APX_PRINTER_ERROR,APX_PRINTER_CANCELLED };
enum apx_printer_error { APX_PRINTER_OK,APX_PRINTER_INVALID,APX_PRINTER_BUSY,
 APX_PRINTER_UNSUPPORTED,APX_PRINTER_TEMPERATURE,APX_PRINTER_TIMEOUT,APX_PRINTER_PAPER,APX_PRINTER_IO,APX_PRINTER_POWER };
struct apx_printer_info {
 uint32_t state,error,row,rows,steps,temperature_adc,temperature_index,paper,completed,cancelled,sequence,battery_mv;
 uint32_t paper_valid,temperature_valid,temperature_limit,heat_pulses,max_heat_count;
 uint32_t head_on,motor_on,power_on;
 uint32_t timer_hz,timer_valid;
 uint32_t adc_channel,sample_adc;
 uint32_t energy_percent;
 uint32_t mode,input_format,plane;
 uint32_t gray_shift_max_counts,gray_off_max_counts;
 uint32_t energy_override; /* 1 when set_energy() replaced the per-mode default */
 /* Per-job half-row timing, in 32768 Hz reference ticks (about 30.5 us). period = time between two motor
  * steps of consecutive half-rows; sample = temperature sampling before a step; work = shift plus heating
  * after a step. Reset at each job start; period_hist counts half-rows by period:
  * <9, <11, <13, <15, <18, <24, >=24 ms. */
 uint32_t period_min,period_max,period_sum,period_count,sample_max,sample_sum,work_max,work_sum,period_hist[7];
 /* Half-rows whose step came later than the configured fixed row period (see board.h), per job. */
 uint32_t period_overruns;
};
/* Camera must be shut down: PE10/PE11 and PD13/PD15 are shared.
 * Packed rows are MSB first; 1=black. Width 1..384, height 1..4096.
 * Borrowed raster remains immutable until DONE/ERROR/CANCELLED. Poll frequently.
 * Heating pulses are bounded synchronous sections; the head is always low
 * when poll returns. No application scheduling delay can extend a pulse. */
int apx_printer_init(void);
/* Sample paper/temperature without moving or heating. Completion releases power.
 * Temperature limit can only be made more restrictive than stock index 65;
 * changing it requires an idle printer. The table index is not a calibrated C reading. */
int apx_printer_check(void);
/* Raw ADC diagnostics only: never feeds or heats, even with invalid samples. */
int apx_printer_probe_adc(unsigned channel);
int apx_printer_set_temperature_limit(unsigned index);
/* Scales every strobe pulse by percent/100 (allowed 100..400); each pulse is still clamped to
 * the 1112-count bound. Idle printer only. Each mode has a board default
 * (APX_PRINTER_ENERGY_GRAY_PERCENT / APX_PRINTER_ENERGY_DOT_PERCENT, 100 when the board does not
 * define them) that apx_printer_set_mode() applies. A value in 100..400 overrides that default
 * for every mode until it is cleared; 0 clears the override and returns to the current mode's
 * default. The override lives in RAM only. */
int apx_printer_set_energy(unsigned percent);
/* Photo/image rendering mode; idle only. Existing packed raster jobs retain
 * their binary input semantics. New image jobs borrow8-bit luma,0=black. */
int apx_printer_set_mode(enum apx_printer_mode mode);
int apx_printer_submit_image(const uint8_t *gray,size_t capacity,unsigned width,unsigned height,unsigned pitch);
int apx_printer_submit(const uint8_t *raster,size_t capacity,unsigned width,unsigned height,unsigned stride);
int apx_printer_feed(unsigned steps);
void apx_printer_poll(void);
void apx_printer_cancel(void);
void apx_printer_shutdown(void);
int apx_printer_busy(void);
void apx_printer_get_info(struct apx_printer_info *info);
/* Caller provides 2*(384+2) signed error cells. Grayscale 0=black, 255=white.
 * Nearest-neighbor scale, fixed-point Floyd-Steinberg diffusion. No heap. */
int apx_printer_dither(const uint8_t *gray,unsigned width,unsigned height,unsigned pitch,
 uint8_t *raster,size_t capacity,unsigned output_width,unsigned output_height,int16_t *errors,size_t error_cells);
uint32_t apx_printer_command(unsigned command,const uint8_t *input,size_t length,uint8_t *output,size_t *output_length);
#endif
