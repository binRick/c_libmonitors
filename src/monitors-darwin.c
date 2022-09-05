#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
#include <Carbon/Carbon.h>
#include <IOKit/graphics/IOGraphicsLib.h>
#include <CoreVideo/CVBase.h>
#include <CoreVideo/CVDisplayLink.h>
#include "monitors.h"
#include "monitors-internal.h"

MODE_DATA{
  int32_t mode_id;
};

MONITOR_DATA{
  CGDirectDisplayID display_id;
};

char *copy_str(char *string){
  char *copy = NULL;
  int count = 0;
  while(string[count] != 0) ++count;
  
  copy = calloc(count, sizeof(char));
  for(; count>=0; --count) copy[count]=string[count];
  return copy;
}

bool process_mode(MODE *mode, CGDisplayModeRef display_mode, CVDisplayLinkRef display_link){
  bool result = false;
  
  uint32_t mode_flags = CGDisplayModeGetIOFlags(display_mode);
  if((mode_flags & kDisplayModeValidFlag)
     && (mode_flags & kDisplayModeSafeFlag)
     && !(mode_flags & kDisplayModeInterlacedFlag)
     && !(mode_flags & kDisplayModeStretchedFlag)){

    CFStringRef format = CGDisplayModeCopyPixelEncoding(display_mode);
    if(!CFStringCompare(format, CFSTR(IO16BitDirectPixels), 0)
       || !CFStringCompare(format, CFSTR(IO32BitDirectPixels), 0)){

      if(mode->_data == NULL)
        mode->_data = calloc(1, sizeof(MODE_DATA));
      mode->_data->mode_id = CGDisplayModeGetIODisplayModeID(display_mode);


      mode->width = (int)CGDisplayModeGetWidth(display_mode);
      mode->height = (int)CGDisplayModeGetHeight(display_mode);
      mode->refresh = (double)CGDisplayModeGetRefreshRate(display_mode);

      // Attempt to recover by calculation if possible
      if(mode->refresh == 0.0){
        const CVTime time = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(display_link);
        if(!(time.flags & kCVTimeIsIndefinite)){
          mode->refresh = (time.timeScale / (double)time.timeValue);
        }
      }
      result = true;
    }
    CFRelease(format);
  }
  
  return result;
}

void detect_modes(MONITOR *monitor){
  int count = 0;
  MODE *modes = NULL;
  
  CGDisplayModeRef current_mode = CGDisplayCopyDisplayMode(monitor->_data->display_id);
  CVDisplayLinkRef display_link;
  CVDisplayLinkCreateWithCGDisplay(monitor->_data->display_id, &display_link);
  CFArrayRef display_modes = CGDisplayCopyAllDisplayModes(monitor->_data->display_id, NULL);
  int mode_count = CFArrayGetCount(display_modes);

  modes = alloc_modes(mode_count);
  for(int i=0; i<mode_count; ++i){
    CGDisplayModeRef display_mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(display_modes, i);
    modes[count].monitor = monitor;
    if(process_mode(&modes[count], display_mode, display_link)){
      if(CGDisplayModeGetIODisplayModeID(display_mode) == CGDisplayModeGetIODisplayModeID(current_mode)){
        monitor->current_mode = &modes[count];
      }
      ++count;
    }
  }

  CFRelease(display_modes);
  CVDisplayLinkRelease(display_link);
  CGDisplayModeRelease(current_mode);

  monitor->mode_count = count;
  monitor->modes = modes;
}

MONITOR *process_monitor(CGDirectDisplayID display){
  if(!CGDisplayIsAsleep(display)){
    MONITOR *monitor = alloc_monitor(sizeof(MONITOR_DATA));

    CFDictionaryRef info = IODisplayCreateInfoDictionary(CGDisplayIOServicePort(display),
                                                         kIODisplayOnlyPreferredName);
    CFDictionaryRef names = CFDictionaryGetValue(info, CFSTR(kDisplayProductName));
    CFStringRef value;
    if(names == NULL || !CFDictionaryGetValueIfPresent(names, CFSTR("en_US"), (const void**) &value)){
      monitor->name = copy_str("Unknown");
    }else{

      CFIndex size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value),
                                                       kCFStringEncodingUTF8);
      char *name = calloc(size+1, sizeof(char));
      CFStringGetCString(value, name, size, kCFStringEncodingUTF8);
      monitor->name = name;
    }
    CFRelease(info);

    monitor->primary = CGDisplayIsMain(display);
    
    const CGSize size = CGDisplayScreenSize(display);
    monitor->width = size.width;
    monitor->height = size.height;
    monitor->id = display;
    monitor->_data->display_id = display;
    CFUUIDRef uuid_ref = CGDisplayCreateUUIDFromDisplayID(display);
    if(uuid_ref){
      CFStringRef uuid_str = CFUUIDCreateString(NULL, uuid_ref);
      if(uuid_str){
        CFIndex uuid_size = CFStringGetMaximumSizeForEncoding(CFStringGetLength(uuid_str),kCFStringEncodingUTF8);
        monitor->uuid = calloc(uuid_size+1, sizeof(char));
        CFStringGetCString(uuid_str, monitor->uuid, uuid_size, kCFStringEncodingUTF8);
        CFRelease(uuid_str);
      }else{
        monitor->uuid = copy_str("Unknown");
      }
      CFRelease(uuid_ref);
    }else{
      monitor->uuid = copy_str("Unknown");
    }
    detect_modes(monitor);
    return monitor;
  }
  return NULL;
}

MONITORS_EXPORT bool libmonitors_detect(int *ext_count, MONITOR ***ext_monitors){  
  MONITOR **monitors = NULL;
  int count = 0;

  uint32_t display_count = 0;
  CGGetOnlineDisplayList(0, NULL, &display_count);
  CGDirectDisplayID *display_ids = calloc(display_count, sizeof(CGDirectDisplayID));
  CGGetOnlineDisplayList(display_count, display_ids, &display_count);
  
  monitors = alloc_monitors(display_count);
  for(uint32_t i=0; i<display_count; ++i){
    MONITOR *monitor = process_monitor(display_ids[i]);
    if(monitor != NULL){
      monitors[count] = monitor;
      ++count;
    }
  }

  free(display_ids);
  
  *ext_monitors = monitors;
  *ext_count = count;
  return true;
}

MONITORS_EXPORT bool libmonitors_make_mode_current(MODE *mode){
  if(mode->monitor->current_mode != mode){
    int success = false;

    CGDirectDisplayID display_id = mode->monitor->_data->display_id;
    
    CVDisplayLinkRef display_link;
    CVDisplayLinkCreateWithCGDisplay(display_id, &display_link);
    CFArrayRef display_modes = CGDisplayCopyAllDisplayModes(display_id, NULL);
    CFIndex mode_count = CFArrayGetCount(display_modes);

    CGDisplayModeRef chosen = NULL;
    for(int i=0; i<mode_count; ++i){
      CGDisplayModeRef display_mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(display_modes, i);
      if(mode->_data->mode_id == CGDisplayModeGetIODisplayModeID(display_mode)){
        chosen = display_mode;
        break;
      }
    }

    if(chosen != NULL){
      if(CGDisplaySetDisplayMode(display_id, chosen, NULL) == kCGErrorSuccess){
        success = true;
      }
    }

    CFRelease(display_modes);
    CVDisplayLinkRelease(display_link);
    
    return success;
  }
  
  return true;
}

MONITORS_EXPORT bool libmonitors_init(){
  return true;
}

MONITORS_EXPORT void libmonitors_deinit(){
}
