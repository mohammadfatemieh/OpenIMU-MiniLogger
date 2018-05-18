#include "gps.h"
#include "minmea.h"
#include <sys/time.h>

namespace
{
    HardwareSerial _gpsSerial(2);
    Adafruit_GPS _gps(&_gpsSerial);


    SemaphoreHandle_t flagMutex = NULL;
    bool timeIsSet = false;
    bool logToSerial = false;

    QueueHandle_t logQueue = NULL;
    SemaphoreHandle_t sdDataReadySemaphore = NULL;

    void gpsRead(void *pvParameters);
    void gpsToSerial();

    time_t computeTime();
    void setTime();
    gpsData_t createDataPoint();
    void setTimeFromGPS(struct minmea_sentence_rmc *rmc);
}

bool GPS::_hasBegun = false;

GPS::GPS()
{

}

GPS::~GPS()
{

}

void GPS::begin()
{
    if(!_hasBegun) {


      Serial.println("Starting GPS.");

        _gps.begin(9600);


        //_gps.sendCommand(PMTK_SET_NMEA_OUTPUT_ALLDATA);
        //_gps.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
        //_gps.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);
        //_gps.sendCommand(PMTK_API_SET_FIX_CTL_1HZ);
        //_gps.sendCommand(PGCMD_ANTENNA);
        //_gps.sendCommand(PGCMD_NOANTENNA);
        //_gpsSerial.println(PMTK_Q_RELEASE);


        flagMutex = xSemaphoreCreateMutex();
        xTaskCreate(&gpsRead, "GPS Read", 2048, NULL, 5, NULL);
    }
}

void GPS::startSerialLogging()
{
    xSemaphoreTake(flagMutex, portMAX_DELAY);
    logToSerial = true;
    xSemaphoreGive(flagMutex);
}

void GPS::stopSerialLogging()
{
    xSemaphoreTake(flagMutex, portMAX_DELAY);
    logToSerial = false;
    xSemaphoreGive(flagMutex);
}

void GPS::startQueueLogging(QueueHandle_t queue, SemaphoreHandle_t semaphore)
{
    xSemaphoreTake(flagMutex, portMAX_DELAY);
    logQueue = queue;
    sdDataReadySemaphore = semaphore;
    xSemaphoreGive(flagMutex);
}

void GPS::stopQueueLogging()
{
    xSemaphoreTake(flagMutex, portMAX_DELAY);
    logQueue = NULL;
    sdDataReadySemaphore = NULL;
    xSemaphoreGive(flagMutex);
}

namespace
{
    void gpsRead(void *pvParameters)
    {
        bool serialEnabled, queueEnabled;
        TickType_t lastTick = xTaskGetTickCount();

        while(1) {


            while(_gpsSerial.available()) {


                //Read one byte
                uint8_t c = _gps.read();
                //Print for debug
                //Serial.write(c);

                if(_gps.newNMEAreceived()) {
                    //_gps.parse(_gps.lastNMEA());
                    char* ptr = _gps.lastNMEA();

                    //There is a bug, sentense starts with \n and the $...
                    ptr++;
                    //Serial.printf("len %i, %s\n", strlen(_gps.lastNMEA()), _gps.lastNMEA());
                    //Serial.printf("%c%c%c%c%c%c\n", ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5]);


                    int id = minmea_sentence_id(ptr, true);
                    //Serial.printf("Sentense id: %i \n", id);

                    switch(id)
                    {
                        //RMC
                        case MINMEA_SENTENCE_RMC:
                          struct minmea_sentence_rmc rmc;
                          if (minmea_parse_rmc(&rmc, ptr))
                          {
                              Serial.println("RMC OK!");
                              if (rmc.valid)
                              {
                                /*
                                struct minmea_float latitude;
                                struct minmea_float longitude;
                                struct minmea_float speed;
                                struct minmea_float course;
                                struct minmea_float variation;
                                */
                                Serial.printf("Date: %2i/%2i/%4i\n", rmc.date.day, rmc.date.month, rmc.date.year);
                                Serial.printf("Time: %2i:%2i:%2i\n", rmc.time.hours, rmc.time.minutes, rmc.time.seconds);
                                setTimeFromGPS(&rmc);

                                float latitude = minmea_tocoord(&rmc.latitude);
                                float longitude = minmea_tocoord(&rmc.longitude);
                                Serial.printf("Latitude %f, Longitude %f \n", latitude, longitude);

                              }

                          }
                        break;

                        case MINMEA_SENTENCE_GGA:
                        struct minmea_sentence_gga gga;
                        if (minmea_parse_gga(&gga, ptr))
                        {
                          /*
                          struct minmea_sentence_gga {
                              struct minmea_time time;
                              struct minmea_float latitude;
                              struct minmea_float longitude;
                              int fix_quality;
                              int satellites_tracked;
                              struct minmea_float hdop;
                              struct minmea_float altitude; char altitude_units;
                              struct minmea_float height; char height_units;
                              int dgps_age;
                          };
                          */
                          int sat = gga.satellites_tracked;
                          float latitude = minmea_tocoord(&gga.latitude);
                          float longitude = minmea_tocoord(&gga.longitude);
                          Serial.printf("Sats: %i, Latitude %f, Longitude %f \n", sat, latitude , longitude );

                        }
                        break;

                    }

                    // Check which modules are enabled
                    xSemaphoreTake(flagMutex, portMAX_DELAY);
                    serialEnabled = logToSerial;
                    queueEnabled = logQueue != NULL;
                    xSemaphoreGive(flagMutex);

                    if (_gps.fix)
                      Serial.println("GPS FIX");

                    // Set system time from GPS
                    if(_gps.fix && !timeIsSet) {
                        setTime();
                        timeIsSet = true;
                    }

                }

            }

            if(xTaskGetTickCount() - lastTick > 1000 / portTICK_RATE_MS) {
                lastTick = xTaskGetTickCount();
                // Log to serial
                if(serialEnabled) {
                    gpsToSerial();
                }

                // Log to queue
                if(queueEnabled) {
                    gpsData_t currentPos = createDataPoint();
                    xSemaphoreTake(flagMutex, portMAX_DELAY);
                    if(logQueue != NULL) {
                        if(xQueueSend(logQueue, &currentPos, 0) == pdTRUE) {
                            xSemaphoreGive(sdDataReadySemaphore);
                        }
                    }
                    xSemaphoreGive(flagMutex);
                }
            }
            vTaskDelay(10/portTICK_RATE_MS);
        }
    }

    gpsData_t createDataPoint()
    {
        gpsData_t data;

        data.fix = _gps.fix;
        data.longitude = _gps.longitude;
        data.latitude = _gps.latitude;
        data.altitude = _gps.altitude;

        return data;
    }

    void gpsToSerial()
    {

        Serial.print("\nTime: ");
        Serial.print(_gps.hour, DEC); Serial.print(':');
        Serial.print(_gps.minute, DEC); Serial.print(':');
        Serial.print(_gps.seconds, DEC); Serial.print('.');
        Serial.println(_gps.milliseconds);
        Serial.print("Date: ");
        Serial.print(_gps.day, DEC); Serial.print('/');
        Serial.print(_gps.month, DEC); Serial.print("/20");
        Serial.println(_gps.year, DEC);
        Serial.print("Fix: "); Serial.print((int)_gps.fix);
        Serial.print(" quality: "); Serial.println((int)_gps.fixquality);

        if (_gps.fix) {
          Serial.print("Location: ");
          Serial.print(_gps.latitude, 4); Serial.print(_gps.lat);
          Serial.print(", ");
          Serial.print(_gps.longitude, 4); Serial.println(_gps.lon);
          Serial.print("Speed (knots): "); Serial.println(_gps.speed);
          Serial.print("Angle: "); Serial.println(_gps.angle);
          Serial.print("Altitude: "); Serial.println(_gps.altitude);
          Serial.print("Satellites: "); Serial.println((int)_gps.satellites);
        }


    }

    time_t computeTime()
    {
        struct tm currentTime;

        currentTime.tm_sec = _gps.seconds;
        currentTime.tm_min = _gps.minute;
        currentTime.tm_hour = _gps.hour;
        currentTime.tm_mday = _gps.day;
        currentTime.tm_mon = _gps.month - 1;
        currentTime.tm_year = _gps.year + 100;

        return mktime(&currentTime);
    }

    void setTime()
    {
        struct timeval timeval;
        timeval.tv_sec = computeTime();
        timeval.tv_usec = 0;

        struct timezone timezone;
        timezone.tz_minuteswest = 0;
        timezone.tz_dsttime = 0;

        settimeofday(&timeval, &timezone);
        Serial.println("Got time from GPS");
    }

    void setTimeFromGPS(struct minmea_sentence_rmc *rmc)
    {
        struct tm currentTime;
        currentTime.tm_sec = rmc->time.seconds;
        currentTime.tm_min = rmc->time.minutes;
        currentTime.tm_hour = rmc->time.hours;
        currentTime.tm_mday = rmc->date.day;
        currentTime.tm_mon = rmc->date.month - 1;
        currentTime.tm_year = rmc->date.year + 100;
        time_t timestamp =  mktime(&currentTime);

        struct timeval timeval;
        timeval.tv_sec = timestamp;
        timeval.tv_usec = 0;

        struct timezone timezone;
        timezone.tz_minuteswest = -5 * 60;
        timezone.tz_dsttime = 0; //DST_CAN;

        settimeofday(&timeval, &timezone);
        Serial.println("Got time from GPS");

    }
}
