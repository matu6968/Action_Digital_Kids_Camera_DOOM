#ifndef APX_BATTERY_INTERNAL_H
#define APX_BATTERY_INTERNAL_H
/* Cooperative driver handoff before a bounded synchronous peripheral section.
 * Drop an in-flight ADC pair, keep completed pairs and the published sample's
 * original validity/timestamp. Normal polling resumes sampling afterwards. */
void apx_battery_defer_conversion(void);
#endif
