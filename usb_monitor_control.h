#ifndef __USB_MONITOR_CONTROL_H__
#define __USB_MONITOR_CONTROL_H__

#include <stdint.h>

#define MAX_CONTROL_RESPONSE_SIZE 48 // 최대 응답 크기

//request_type for root device
#define MONITOR_REQUEST_TYPE_GET_VERSION 1  // get monitor version
#define MONITOR_REQUEST_TYPE_UPDATE_FIRMWARE 2  // update firmware, wValue는 packet number임 0부터 시작. 

//request_type for screen
#define SCREEN_REQUEST_TYPE_OFFSET_RESET 1 // reset screen frame data offset
#define SCREEN_REQUEST_TYPE_GET_SCREEN_INFO 2 // get screen info
#define SCREEN_REQUEST_TYPE_SET_SCREEN_DEFAULT_IMAGE 3 // 화면을 기본 화면으로 그리기

//request_type for touch
#define TOUCH_REQUEST_TYPE_RESET 100  // reset touch controller. HID의 bRequest값과 분리하기 위해 100번 이상으로 한다.

#define SCREEN_PIXEL_FORMAT_RGB565 0x01 // RGB565 pixel format

//response codes from root device
//response codes shared by all request types
#define USB_MONITOR_RESPONSE_CODE_OK 0 // 응답 코드: 성공
#define USB_MONITOR_RESPONSE_CODE_UKNOWN_REQUEST 1 // 응답 코드: 알 수 없는 요청
#define USB_SCREEN_RESPONSE_CODE_UNNOWN_LCD_BOARD 1001

typedef struct USB_MONITOR_CONTROL_RESPONSE_GENERIC {
    uint16_t response_code; // 0:ok, any other value: fail_code
    uint8_t request_type;  // see request types above
    uint8_t reserved[MAX_CONTROL_RESPONSE_SIZE-3]; // reserved for future use
} __attribute__((packed)) usb_monitor_control_response_generic_t;

typedef struct USB_MONITOR_CONTROL_RESPONSE_GET_VERSION {
    uint16_t response_code; // 0:ok, any other value: fail_code
    uint8_t request_type;  // see request types above
    uint32_t major_version; // major version
    uint32_t minor_version; // minor version
    uint32_t patch_version; // patch version
} __attribute__((packed)) usb_monitor_control_response_get_version_t;


#define USB_SCREEN_PIXEL_FORMAT_RGB565 0x01 // RGB565 pixel format

typedef struct USB_MONITOR_CONTROL_RESPONSE_SCREEN_INFO {
    uint16_t response_code;      // 0:ok, any other value: fail_code
    uint8_t request_type;       // see request types above
    uint16_t screen_width;       // width of the screen
    uint16_t screen_height;      // height of the screen
    uint8_t  screen_pixel_format;// pixel format of the screen
} __attribute__((packed)) usb_monitor_control_response_screen_info_t;

// 공통 응답 union
typedef union USB_MONITOR_CONTROL_RESPONSE {
    usb_monitor_control_response_generic_t     generic;
    usb_monitor_control_response_screen_info_t screen_info;
    usb_monitor_control_response_get_version_t version;
} __attribute__((packed)) usb_monitor_control_response_t;

// union 크기가 MAX_CONTROL_RESPONSE_SIZE 이하인지 확인
#define STATIC_ASSERT(COND,MSG) typedef char static_assertion_##MSG[(COND)?1:-1]

// union 크기가 MAX_CONTROL_RESPONSE_SIZE 이하인지 확인
STATIC_ASSERT(sizeof(usb_monitor_control_response_t) <= MAX_CONTROL_RESPONSE_SIZE,
              response_union_too_big);

#define UPDATE_FIRMWARE_PACKET_SIZE 1024 // firmware update packet size. libusb에서 1024를 넘지 못하게 하므로, 1024로 한다.

typedef struct FIRMWARE_UPDATE_DATA{
    uint32_t data_size; // 아래에 있는 실제 data size, data_size와 crc는 제외한 크기이다. 
    uint32_t crc; // crc32c checksum of the data
    int major_version; // major version of the firmware. major version값이 0보다 작으면, version정보가 없는 케이스이다. 이 케이스에서는 Device측에서는 실제 binary를 실행해봐야 버전을 알수 있다.
    int minor_version; // minor version of the firmware
    int patch_version; // patch version of the firmware
    uint8_t data[];   //data point가 반드시 32bit 단위로 정렬되어야 한다. 따라서 data앞에 있는 변수들은 반드시 32bit 단위로 정렬되어야 한다.
}__attribute__((packed)) firmware_update_data_t;

#endif // __USB_MONITOR_CONTROL_H__