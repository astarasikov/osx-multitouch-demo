#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOMessage.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDShared.h>
#include <IOKit/hidsystem/IOHIDParameter.h>

#include "touch_shared.h"

#define TOUCH_SCREEN 1

//---------------------------------------------------------------------------
// Globals
//---------------------------------------------------------------------------
static IONotificationPortRef	gNotifyPort = NULL;
static io_iterator_t		gAddedIter = 0;

//---------------------------------------------------------------------------
// TypeDefs
//---------------------------------------------------------------------------
typedef enum CalibrationState {
    kCalibrationStateInactive   = 0,
    kCalibrationStateTopLeft,
    kCalibrationStateTopRight,
    kCalibrationStateBottomRight,
    kCalibrationStateBottomLeft
} CalibrationState;

typedef struct HIDData
{
    io_object_t			notification;
    IOHIDDeviceInterface122 ** 	hidDeviceInterface;
    IOHIDQueueInterface **      hidQueueInterface;
    CFDictionaryRef             hidElementDictionary;
    CFRunLoopSourceRef 		eventSource;
    CalibrationState            state;
    SInt32                      minx;
    SInt32                      maxx;
    SInt32                      miny;
    SInt32                      maxy;
    UInt8                       buffer[256];
} HIDData;

typedef HIDData * 		HIDDataRef;

typedef struct HIDElement {
    SInt32		currentValue;
    SInt32		usagePage;
    SInt32		usage;
    IOHIDElementType	type;
    IOHIDElementCookie	cookie;
    HIDDataRef          owner;
}HIDElement;

static const char *translateHIDType(IOHIDElementType type) {
    switch (type) {
        case 1:
            return "MISC";
        case 2:
            return "Button";
        case 3:
            return "Axis";
        case 4:
            return "ScanCodes";
        case 129:
            return "Output";
        case 257:
            return "Feature";
        case 513:
            return "Collection";

        default:
            return "unknown";
            break;
    }
};

static void reportHidElement(HIDElement *element) {
    if (!element) {
        return;
    }

    static int finger_number = 0;

    const char *hidType = translateHIDType(element->type);
    const char *hidUsage = "unknown";
    if (element->usagePage == 0xd) {
        switch (element->usage) {
            case kHIDUsage_Dig_TouchScreen:
                hidUsage = "touch screen";
                break;
            case 1:
                hidUsage = "digitizer";
                break;
            case 2:
                hidUsage = "pen";
                break;
            case 0x20:
                hidUsage = "stylus";
                break;
            case 0x22:
                hidUsage = "finger";
                break;
            case 0x30:
                hidUsage = "pressure";
                break;
            case 0x32:
                hidUsage = "in-range";
                break;
                //case 0x33:
            case kHIDUsage_Dig_Touch:
                hidUsage = "touch";
                break;
            case 0x48:
                hidUsage = "width";
                break;
            case 0x49:
                hidUsage = "height";
                break;
            case 0x51:
                hidUsage = "contact identifier";
                finger_number = element->currentValue;
                break;
            case 0x53:
                hidUsage = "device index";
                break;
            case 0x54:
                hidUsage = "actual touch count";
                break;
            case 0x55:
                hidUsage = "contact count maximum";
                break;
        }
    }
#if TOUCH_REPORT
    printf("usage page %x usage %x type %s %s value 0x%x (%d)\n",
           element->usagePage, element->usage,
           hidType,
           hidUsage,
           element->currentValue,
           element->currentValue);
#endif
    if (element->usagePage == 1) {
        float scale_x = 1920 / 32768.0f;
        float scale_y = 1080 / 32768.0f;

        short value = element->currentValue & 0xffff;

        if (element->usage == kHIDUsage_GD_X) {
            int x = (int)(value * scale_x);
            submitTouch((struct TouchEvent){finger_number, x, 0});
        }
        else if (element->usage == kHIDUsage_GD_Y) {
            int y = (int)(value * scale_y);
            submitTouch((struct TouchEvent){finger_number, 0, y});
        }
    }
}

typedef HIDElement * 		HIDElementRef;

#ifndef max
#define max(a, b) \
((a > b) ? a:b)
#endif

#ifndef min
#define min(a, b) \
((a < b) ? a:b)
#endif

//---------------------------------------------------------------------------
// Methods
//---------------------------------------------------------------------------
static void InitHIDNotifications();
static void HIDDeviceAdded(void *refCon, io_iterator_t iterator);
static void DeviceNotification(void *refCon, io_service_t service, natural_t messageType, void *messageArgument);
static bool FindHIDElements(HIDDataRef hidDataRef);
#ifdef TOUCH_SCREEN
static bool SetupQueue(HIDDataRef hidDataRef);
static void QueueCallbackFunction(
                                  void * 			target,
                                  IOReturn 			result,
                                  void * 			refcon,
                                  void * 			sender);
#endif
static void InterruptReportCallbackFunction
(void *	 		target,
 IOReturn 		result,
 void * 			refcon,
 void * 			sender,
 uint32_t		 	bufferSize);

void startTouchLoop(void) {
        InitHIDNotifications();
        //CFRunLoopRun();
}


//---------------------------------------------------------------------------
// InitHIDNotifications
//
// This routine just creates our master port for IOKit and turns around
// and calls the routine that will alert us when a HID Device is plugged in.
//---------------------------------------------------------------------------

static void InitHIDNotifications()
{
    CFMutableDictionaryRef 	matchingDict;
    CFNumberRef                 refProdID;
    CFNumberRef                 refVendorID;
    SInt32                      productID = TOUCH_PID;
    SInt32                      vendorID = TOUCH_VID;
    mach_port_t 		masterPort;
    kern_return_t		kr;

    // first create a master_port for my task
    //
    kr = IOMasterPort(bootstrap_port, &masterPort);
    if (kr || !masterPort)
        return;

    // Create a notification port and add its run loop event source to our run loop
    // This is how async notifications get set up.
    //
    gNotifyPort = IONotificationPortCreate(masterPort);
    CFRunLoopAddSource(	CFRunLoopGetCurrent(),
                       IONotificationPortGetRunLoopSource(gNotifyPort),
                       kCFRunLoopDefaultMode);

    // Create the IOKit notifications that we need
    //
#ifdef TOUCH_SCREEN
    /* Create a matching dictionary that (initially) matches all HID devices. */
    matchingDict = IOServiceMatching(kIOHIDDeviceKey);

    if (!matchingDict)
        return;

    /* Create objects for product and vendor IDs. */
    refProdID = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &productID);
    refVendorID = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &vendorID);

    /* Add objects to matching dictionary and clean up. */
    CFDictionarySetValue (matchingDict, CFSTR (kIOHIDVendorIDKey), refVendorID);
    CFDictionarySetValue (matchingDict, CFSTR (kIOHIDProductIDKey), refProdID);

    CFRelease(refProdID);
    CFRelease(refVendorID);

#else
    /* Create a matching dictionary that (initially) matches all HID devices. */
    matchingDict = IOServiceMatching("IOHIDDevice");

    if (!matchingDict)
        return;

    /* Create objects for product and vendor IDs. */
    refProdID = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &productID);
    refVendorID = CFNumberCreate (kCFAllocatorDefault, kCFNumberIntType, &vendorID);

    /* Add objects to matching dictionary and clean up. */
    CFDictionarySetValue (matchingDict, CFSTR (kIOHIDPrimaryUsageKey), refVendorID);
    CFDictionarySetValue (matchingDict, CFSTR (kIOHIDPrimaryUsagePageKey), refProdID);

    CFRelease(refProdID);
    CFRelease(refVendorID);
    //if (!matchingDict)
    //return;
    //CFDictionarySetValue (matchingDict, CFSTR (kIOHIDProductKey), CFSTR ("XSKey"));

#endif

    // Now set up a notification to be called when a device is first matched by I/O Kit.
    // Note that this will not catch any devices that were already plugged in so we take
    // care of those later.
    kr = IOServiceAddMatchingNotification(gNotifyPort,			// notifyPort
                                          kIOFirstMatchNotification,	// notificationType
                                          matchingDict,			// matching
                                          HIDDeviceAdded,		// callback
                                          NULL,				// refCon
                                          &gAddedIter			// notification
                                          );

    if ( kr != kIOReturnSuccess )
        return;

    HIDDeviceAdded( NULL, gAddedIter );
}

//---------------------------------------------------------------------------
// HIDDeviceAdded
//
// This routine is the callback for our IOServiceAddMatchingNotification.
// When we get called we will look at all the devices that were added and
// we will:
//
// Create some private data to relate to each device
//
// Submit an IOServiceAddInterestNotification of type kIOGeneralInterest for
// this device using the refCon field to store a pointer to our private data.
// When we get called with this interest notification, we can grab the refCon
// and access our private data.
//---------------------------------------------------------------------------

static void HIDDeviceAdded(void *refCon, io_iterator_t iterator)
{
    io_object_t 		hidDevice 		= 0;
    IOCFPlugInInterface **	plugInInterface 	= NULL;
    IOHIDDeviceInterface122 **	hidDeviceInterface 	= NULL;
    HRESULT 			result 			= S_FALSE;
    HIDDataRef                  hidDataRef              = NULL;
    IOReturn			kr;
    SInt32 			score;
    bool                        pass;

    /* Interate through all the devices that matched */
    while ((hidDevice = IOIteratorNext(iterator)))
    {
        // Create the CF plugin for this device
        kr = IOCreatePlugInInterfaceForService(hidDevice, kIOHIDDeviceUserClientTypeID,
                                               kIOCFPlugInInterfaceID, &plugInInterface, &score);

        if ( kr != kIOReturnSuccess )
            goto HIDDEVICEADDED_NONPLUGIN_CLEANUP;

        /* Obtain a device interface structure (hidDeviceInterface). */
        result = (*plugInInterface)->QueryInterface(plugInInterface, CFUUIDGetUUIDBytes(kIOHIDDeviceInterfaceID122),
                                                    (LPVOID *)&hidDeviceInterface);

        // Got the interface
        if ( ( result == S_OK ) && hidDeviceInterface )
        {
            /* Create a custom object to keep data around for later. */
            hidDataRef = (HIDData*)malloc(sizeof(HIDData));
            bzero(hidDataRef, sizeof(HIDData));

            hidDataRef->hidDeviceInterface = hidDeviceInterface;

#ifdef TOUCH_SCREEN
            /* Open the device interface. */
            result = (*(hidDataRef->hidDeviceInterface))->open (hidDataRef->hidDeviceInterface, kIOHIDOptionsTypeSeizeDevice);

            if (result != S_OK)
                goto HIDDEVICEADDED_FAIL;

            /* Find the HID elements for this device and set up a receive queue. */
            pass = FindHIDElements(hidDataRef);
            pass = SetupQueue(hidDataRef);

            printf("Please touch screen to continue.\n\n");
#else
            /* Check for Bluetooth HID descriptors first. */

            /* Create a CFArray object based on the IORegistry entry for this HID Device. */
            CFArrayRef arrayRef = IORegistryEntryCreateCFProperty(hidDevice, CFSTR("HIDDescriptor"), 0, 0);

            if ( arrayRef )
            {
                CFShow(arrayRef);
                arrayRef = CFArrayGetValueAtIndex(arrayRef, 0);

                if ( arrayRef )
                {
                    CFShow(arrayRef);
                    CFDataRef data = CFArrayGetValueAtIndex(arrayRef, 1);

                    CFShow(data);

                    if ( data )
                    {
                        const UInt8 * buffer = CFDataGetBytePtr(data);
                        int i;
                        printf("Bluetooth HID descriptor: ");
                        for (i=0; i<CFDataGetLength(data); i++)
                            printf("0x%x, ", buffer[i]);
                        printf("\n");
                    }
                }
            }

            /* Open the device. */
            result = (*(hidDataRef->hidDeviceInterface))->open (hidDataRef->hidDeviceInterface, 0);

            /* Find the HID elements for this device */
            pass = FindHIDElements(hidDataRef);

            /* Create an asynchronous event source for this device. */
            result = (*(hidDataRef->hidDeviceInterface))->createAsyncEventSource(hidDataRef->hidDeviceInterface, &hidDataRef->eventSource);

            /* Set the handler to call when the device sends a report. */
            result = (*(hidDataRef->hidDeviceInterface))->setInterruptReportHandlerCallback(hidDataRef->hidDeviceInterface, hidDataRef->buffer, sizeof(hidDataRef->buffer), &InterruptReportCallbackFunction, NULL, hidDataRef);

            /* Add the asynchronous event source to the run loop. */
            CFRunLoopAddSource(CFRunLoopGetCurrent(), hidDataRef->eventSource, kCFRunLoopDefaultMode);

#endif

            /* Register an interest in finding out anything that happens with this device (disconnection, for example) */
            IOServiceAddInterestNotification(
                                             gNotifyPort,		// notifyPort
                                             hidDevice,			// service
                                             kIOGeneralInterest,		// interestType
                                             DeviceNotification,		// callback
                                             hidDataRef,			// refCon
                                             &(hidDataRef->notification)	// notification
                                             );

            goto HIDDEVICEADDED_CLEANUP;
        }

#ifdef TOUCH_SCREEN
    HIDDEVICEADDED_FAIL:
#endif
        // Failed to allocated a UPS interface.  Do some cleanup
        if ( hidDeviceInterface )
        {
            (*hidDeviceInterface)->Release(hidDeviceInterface);
            hidDeviceInterface = NULL;
        }

        if ( hidDataRef )
            free ( hidDataRef );

    HIDDEVICEADDED_CLEANUP:
        // Clean up
        (*plugInInterface)->Release(plugInInterface);

    HIDDEVICEADDED_NONPLUGIN_CLEANUP:
        IOObjectRelease(hidDevice);
    }
}

//---------------------------------------------------------------------------
// DeviceNotification
//
// This routine will get called whenever any kIOGeneralInterest notification
// happens.
//---------------------------------------------------------------------------

static void DeviceNotification(void *		refCon,
                        io_service_t 	service,
                        natural_t 	messageType,
                        void *		messageArgument )
{
    kern_return_t	kr;
    HIDDataRef		hidDataRef = (HIDDataRef) refCon;

    /* Check to see if a device went away and clean up. */
    if ( (hidDataRef != NULL) &&
        (messageType == kIOMessageServiceIsTerminated) )
    {
        if (hidDataRef->hidQueueInterface != NULL)
        {
            kr = (*(hidDataRef->hidQueueInterface))->stop((hidDataRef->hidQueueInterface));
            kr = (*(hidDataRef->hidQueueInterface))->dispose((hidDataRef->hidQueueInterface));
            kr = (*(hidDataRef->hidQueueInterface))->Release (hidDataRef->hidQueueInterface);
            hidDataRef->hidQueueInterface = NULL;
        }

        if (hidDataRef->hidDeviceInterface != NULL)
        {
            kr = (*(hidDataRef->hidDeviceInterface))->close (hidDataRef->hidDeviceInterface);
            kr = (*(hidDataRef->hidDeviceInterface))->Release (hidDataRef->hidDeviceInterface);
            hidDataRef->hidDeviceInterface = NULL;
        }

        if (hidDataRef->notification)
        {
            kr = IOObjectRelease(hidDataRef->notification);
            hidDataRef->notification = 0;
        }

    }
}

//---------------------------------------------------------------------------
// FindHIDElements
//---------------------------------------------------------------------------
static bool FindHIDElements(HIDDataRef hidDataRef)
{
    CFArrayRef              elementArray	= NULL;
    CFMutableDictionaryRef  hidElements     = NULL;
    CFMutableDataRef        newData         = NULL;
    CFNumberRef             number		= NULL;
    CFDictionaryRef         element		= NULL;
    HIDElement              newElement;
    IOReturn                ret		= kIOReturnError;
    unsigned                i;

    if (!hidDataRef)
        return false;

    /* Create a mutable dictionary to hold HID elements. */
    hidElements = CFDictionaryCreateMutable(
                                            kCFAllocatorDefault,
                                            0,
                                            &kCFTypeDictionaryKeyCallBacks,
                                            &kCFTypeDictionaryValueCallBacks);
    if ( !hidElements )
        return false;

    // Let's find the elements
    ret = (*hidDataRef->hidDeviceInterface)->copyMatchingElements(
                                                                  hidDataRef->hidDeviceInterface,
                                                                  NULL,
                                                                  &elementArray);


    if ( (ret != kIOReturnSuccess) || !elementArray)
        goto FIND_ELEMENT_CLEANUP;

    //CFShow(elementArray);

    /* Iterate through the elements and read their values. */
    for (i=0; i<CFArrayGetCount(elementArray); i++)
    {
        element = (CFDictionaryRef) CFArrayGetValueAtIndex(elementArray, i);
        if ( !element )
            continue;

        bzero(&newElement, sizeof(HIDElement));

        newElement.owner = hidDataRef;

        /* Read the element's usage page (top level category describing the type of
         element---kHIDPage_GenericDesktop, for example) */
        number = (CFNumberRef)CFDictionaryGetValue(element, CFSTR(kIOHIDElementUsagePageKey));
        if ( !number ) continue;
        CFNumberGetValue(number, kCFNumberSInt32Type, &newElement.usagePage );

        /* Read the element's usage (second level category describing the type of
         element---kHIDUsage_GD_Keyboard, for example) */
        number = (CFNumberRef)CFDictionaryGetValue(element, CFSTR(kIOHIDElementUsageKey));
        if ( !number ) continue;
        CFNumberGetValue(number, kCFNumberSInt32Type, &newElement.usage );

        /* Read the cookie (unique identifier) for the element */
        number = (CFNumberRef)CFDictionaryGetValue(element, CFSTR(kIOHIDElementCookieKey));
        if ( !number ) continue;
        CFNumberGetValue(number, kCFNumberIntType, &(newElement.cookie) );

        /* Determine what type of element this is---button, Axis, etc. */
        number = (CFNumberRef)CFDictionaryGetValue(element, CFSTR(kIOHIDElementTypeKey));
        if ( !number ) continue;
        CFNumberGetValue(number, kCFNumberIntType, &(newElement.type) );

        /* Pay attention to X/Y coordinates of a pointing device and
         the first mouse button.  For other elements, go on to the
         next element. */

        reportHidElement(&newElement);

        //type d kHIDPage_Digitizer
        //type 1 gd_x, gd_y
        //vendor defined ff00
        if ( newElement.usagePage == kHIDPage_GenericDesktop )
        {
            switch ( newElement.usage )
            {
                case kHIDUsage_GD_X:
                case kHIDUsage_GD_Y:
                    break;
                default:
                    continue;
            }
        }
        else if ( newElement.usagePage == kHIDPage_Button )
        {

            switch ( newElement.usage )
            {
                case kHIDUsage_Button_1:
                    break;
                default:
                    continue;
            }
        }
        else if ( newElement.usagePage == kHIDPage_Digitizer )
        {
            switch (newElement.usage) {
                case kHIDUsage_Dig_TouchScreen:
                    printf("touch screen\n");
                    break;
                case kHIDUsage_Dig_Touch:
                    printf("touch\n");
                    break;
                case 0x51:
                    printf("contact identifier\n");
                    break;
                case 0x32:
                    printf("in-range\n");
                    break;
                case 0x55:
                    printf("contact count maximum\n");
                    break;
                case 0x30:
                    printf("pressure\n");
                    break;
                case 0x48:
                    printf("width\n");
                    break;
                case 0x49:
                    printf("height\n");
                    break;
                case 0x53:
                    printf("device index\n");
                    break;
                case 0x54:
                    printf("actual touch count\n");
                    break;

                default:
                    continue;
            }
        }
        else
            continue;

        /* Add this element to the hidElements dictionary. */
        newData = CFDataCreateMutable(kCFAllocatorDefault, sizeof(HIDElement));
        if ( !newData ) continue;
        bcopy(&newElement, CFDataGetMutableBytePtr(newData), sizeof(HIDElement));

        number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &newElement.cookie);
        if ( !number )  continue;
        CFDictionarySetValue(hidElements, number, newData);
        CFRelease(number);
        CFRelease(newData);
    }

FIND_ELEMENT_CLEANUP:
    if ( elementArray ) CFRelease(elementArray);

    if (CFDictionaryGetCount(hidElements) == 0)
    {
        CFRelease(hidElements);
        hidElements = NULL;
    }
    else
    {
        hidDataRef->hidElementDictionary = hidElements;
    }

    return hidDataRef->hidElementDictionary;
}

#ifdef TOUCH_SCREEN
//---------------------------------------------------------------------------
// SetupQueue
//---------------------------------------------------------------------------
static bool SetupQueue(HIDDataRef hidDataRef)
{
    CFIndex		count 		= 0;
    CFIndex		i 		= 0;
    CFMutableDataRef *	elements	= NULL;
    CFStringRef *	keys		= NULL;
    IOReturn		ret;
    HIDElementRef	tempHIDElement	= NULL;
    bool		cookieAdded 	= false;
    bool                boolRet         = true;

    if ( !hidDataRef->hidElementDictionary || (((count = CFDictionaryGetCount(hidDataRef->hidElementDictionary)) <= 0)))
        return false;

    keys 	= (CFStringRef *)malloc(sizeof(CFStringRef) * count);
    elements 	= (CFMutableDataRef *)malloc(sizeof(CFMutableDataRef) * count);

    CFDictionaryGetKeysAndValues(hidDataRef->hidElementDictionary, (const void **)keys, (const void **)elements);

    hidDataRef->hidQueueInterface = (*hidDataRef->hidDeviceInterface)->allocQueue(hidDataRef->hidDeviceInterface);
    if ( !hidDataRef->hidQueueInterface )
    {
        boolRet = false;
        goto SETUP_QUEUE_CLEANUP;
    }

    ret = (*hidDataRef->hidQueueInterface)->create(hidDataRef->hidQueueInterface, 0, 8);
    if (ret != kIOReturnSuccess)
    {
        boolRet = false;
        goto SETUP_QUEUE_CLEANUP;
    }

    for (i=0; i<count; i++)
    {
        if ( !elements[i] ||
            !(tempHIDElement = (HIDElementRef)CFDataGetMutableBytePtr(elements[i])))
            continue;

        reportHidElement(tempHIDElement);

        if ((tempHIDElement->type < kIOHIDElementTypeInput_Misc) || (tempHIDElement->type > kIOHIDElementTypeInput_ScanCodes))
            continue;

        ret = (*hidDataRef->hidQueueInterface)->addElement(hidDataRef->hidQueueInterface, tempHIDElement->cookie, 0);

        if (ret == kIOReturnSuccess)
            cookieAdded = true;
    }

    if ( cookieAdded )
    {
        ret = (*hidDataRef->hidQueueInterface)->createAsyncEventSource(hidDataRef->hidQueueInterface, &hidDataRef->eventSource);
        if ( ret != kIOReturnSuccess )
        {
            boolRet = false;
            goto SETUP_QUEUE_CLEANUP;
        }

        ret = (*hidDataRef->hidQueueInterface)->setEventCallout(hidDataRef->hidQueueInterface, QueueCallbackFunction, NULL, hidDataRef);
        if ( ret != kIOReturnSuccess )
        {
            boolRet = false;
            goto SETUP_QUEUE_CLEANUP;
        }

        CFRunLoopAddSource(CFRunLoopGetCurrent(), hidDataRef->eventSource, kCFRunLoopDefaultMode);

        ret = (*hidDataRef->hidQueueInterface)->start(hidDataRef->hidQueueInterface);
        if ( ret != kIOReturnSuccess )
        {
            boolRet = false;
            goto SETUP_QUEUE_CLEANUP;
        }
    }
    else
    {
        (*hidDataRef->hidQueueInterface)->stop(hidDataRef->hidQueueInterface);
        (*hidDataRef->hidQueueInterface)->dispose(hidDataRef->hidQueueInterface);
        (*hidDataRef->hidQueueInterface)->Release(hidDataRef->hidQueueInterface);
        hidDataRef->hidQueueInterface = NULL;
    }

SETUP_QUEUE_CLEANUP:

    free(keys);
    free(elements);

    return boolRet;
}


//---------------------------------------------------------------------------
// QueueCallbackFunction
//---------------------------------------------------------------------------
static void QueueCallbackFunction(
                           void * 			target,
                           IOReturn 			result,
                           void * 			refcon,
                           void * 			sender)
{
    HIDDataRef          hidDataRef      = (HIDDataRef)refcon;
    AbsoluteTime 	zeroTime 	= {0,0};
    CFNumberRef		number		= NULL;
    CFMutableDataRef	element		= NULL;
    HIDElementRef	tempHIDElement  = NULL;//(HIDElementRef)refcon;
    IOHIDEventStruct 	event;
    bool                change;

    if ( !hidDataRef || ( sender != hidDataRef->hidQueueInterface))
        return;

    while (result == kIOReturnSuccess)
    {
        result = (*hidDataRef->hidQueueInterface)->getNextEvent(
                                                                hidDataRef->hidQueueInterface,
                                                                &event,
                                                                zeroTime,
                                                                0);

        if ( result != kIOReturnSuccess )
            continue;

        // Only intersted in 32 values right now
        if ((event.longValueSize != 0) && (event.longValue != NULL))
        {
            free(event.longValue);
            continue;
        }

        number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &event.elementCookie);
        if ( !number )  continue;
        element = (CFMutableDataRef)CFDictionaryGetValue(hidDataRef->hidElementDictionary, number);
        CFRelease(number);

        if ( !element ||
            !(tempHIDElement = (HIDElement *)CFDataGetMutableBytePtr(element)))
            continue;

        change = (tempHIDElement->currentValue != event.value);
        tempHIDElement->currentValue = event.value;

        reportHidElement(tempHIDElement);
    }

}
#endif


//---------------------------------------------------------------------------
// InterruptReportCallbackFunction
//---------------------------------------------------------------------------
static void InterruptReportCallbackFunction
(void *	 		target,
 IOReturn 		result,
 void * 			refcon,
 void * 			sender,
 uint32_t		 	bufferSize)
{
    HIDDataRef hidDataRef = (HIDDataRef)refcon;
    int index;

    if ( !hidDataRef )
        return;

    printf("Buffer = ");

    for ( index=0; index<bufferSize; index++)
        printf("%2.2x ", hidDataRef->buffer[index]);

    printf("\n");

}
