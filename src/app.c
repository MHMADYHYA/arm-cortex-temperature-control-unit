/***************************************************************************//**
 * @file
 * @brief Top level application functions
 *******************************************************************************/

#include "sl_segmentlcd.h"
#include "i2cspm.h"

// הצהרה על המשתנה temperature כגלובלי
/***************************************************************************//**
 * Initialize application.
 ******************************************************************************/
void app_init(void)
{
    // אתחול ה-LCD
    //sl_segment_lcd_init(false);
    //sl_segment_lcd_write("Init");

    // אתחול פונקציות אחרות
    i2cspm_app_init();
}

/***************************************************************************//**
 * App ticking function.
 ******************************************************************************/
void app_process_action(void)
{
  i2cspm_app_process_action();

}
    // פעולה עיקרית שמבוצעת כל הזמן
    //i2cspm_app_process_action();

    // לולאה שמעדכנת את הטמפרטורה כל הזמן
    //while (1) {
        // קריאה מחדש של הטמפרטורה (הפונקציה i2cspm_app_process_action כבר קוראת מהחיישן)



        // הצגת הטמפרטורה על המסך
       //char temp_str[8];
       //snprintf(temp_str,sizeof(temp_str),"%ld temp",temperature);
        //int lcd_temp = temperature / 100; // המרה ל-Celsius
        //sl_segment_lcd_write(temp_str); // הצגת הטמפרטורה על המסך


