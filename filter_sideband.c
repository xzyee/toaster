/*++

Module Name:

    filter.c

Abstract:

    This module shows how to a write a sideband filter driver. The driver creates
    a control device object, which represents a legacy non-Plug and Play device or 
    control interface through which a Plug and Play driver receives so-called 
    "sideband" I/O requests. This control object is not a part of the Plug and Play
    and there is one control object for all instances of the device.
    The module also shows using a collection to keep track of all device objects. 
    It is very important to delete the control device object so that driver can 
    unload when the filter device is deleted.

    An alternative approach is to enumerate a raw PDO for every device
    the filter attaches to so that it can provide a direct sideband communication 
    with the usermode application. The KbFilter driver demonstrates that approach.

    

Environment:

    Kernel mode

--*/

#include "filter.h"

//
// Collection object is used to store all the FilterDevice objects so
// that any event callback routine can easily walk thru the list and pick a
// specific instance of the device for filtering.
//
WDFCOLLECTION   FilterDeviceCollection;
WDFWAITLOCK     FilterDeviceCollectionLock;


#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, FilterEvtDeviceAdd)
#pragma alloc_text (PAGE, FilterEvtDeviceContextCleanup)
#endif


//
// ControlDevice provides a sideband communication to the filter from
// usermode. This is required if the filter driver is sitting underneath
// another driver that fails custom ioctls defined by the Filter driver.
// Since there is one control-device for all instances of the device the
// filter is attached to, we will store the device handle in a global variable.
//

WDFDEVICE       ControlDevice = NULL;

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, FilterEvtIoDeviceControl)
#pragma alloc_text (PAGE, FilterCreateControlDevice)
#pragma alloc_text (PAGE, FilterDeleteControlDevice)
#endif

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT  DriverObject,
    PUNICODE_STRING RegistryPath
    )
/*++

Routine Description:

    Installable driver initialization entry point.
    This entry point is called directly by the I/O system.

Arguments:

    DriverObject - pointer to the driver object

    RegistryPath - pointer to a unicode string representing the path,
                   to driver-specific key in the registry.

Return Value:

    STATUS_SUCCESS if successful,
    STATUS_UNSUCCESSFUL otherwise.

--*/
{
    WDF_DRIVER_CONFIG   config;
    NTSTATUS            status;
    WDFDRIVER   hDriver;

    KdPrint(("Toaster SideBand Filter Driver Sample - Driver Framework Edition.\n"));

    //
    // Initiialize driver config to control the attributes that
    // are global to the driver. Note that framework by default
    // provides a driver unload routine. If you create any resources
    // in the DriverEntry and want to be cleaned in driver unload,
    // you can override that by manually setting the EvtDriverUnload in the
    // config structure. In general xxx_CONFIG_INIT macros are provided to
    // initialize most commonly used members.
    //

    WDF_DRIVER_CONFIG_INIT(
        &config,
        FilterEvtDeviceAdd
    );

    //
    // Create a framework driver object to represent our driver.
    //
    status = WdfDriverCreate(DriverObject,
                            RegistryPath,
                            WDF_NO_OBJECT_ATTRIBUTES,
                            &config,
                            &hDriver);
    if (!NT_SUCCESS(status)) {
        KdPrint( ("WdfDriverCreate failed with status 0x%x\n", status));
    }

    //
    // Since there is only one control-device for all the instances
    // of the physical device, we need an ability to get to particular instance
    // of the device in our FilterEvtIoDeviceControlForControl. For that we
    // will create a collection object and store filter device objects.        
    // The collection object has the driver object as a default parent.
    //

    status = WdfCollectionCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                &FilterDeviceCollection/*输出，全局变量*/);
    if (!NT_SUCCESS(status))
    {
        KdPrint( ("WdfCollectionCreate failed with status 0x%x\n", status));
        return status;
    }

    //
    // The wait-lock object has the driver object as a default parent.
    //

    status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES,
                                &FilterDeviceCollectionLock/*输出，全局变量*/);
    if (!NT_SUCCESS(status))
    {
        KdPrint( ("WdfWaitLockCreate failed with status 0x%x\n", status));
        return status;
    }

    return status;
}

_Use_decl_annotations_
NTSTATUS
FilterEvtDeviceAdd(
    WDFDRIVER        Driver,
    PWDFDEVICE_INIT  DeviceInit
    )
/*++
Routine Description:

    EvtDeviceAdd is called by the framework in response to AddDevice
    call from the PnP manager. Here you can query the device properties
    using WdfFdoInitWdmGetPhysicalDevice/IoGetDeviceProperty and based
    on that, decide to create a filter device object and attach to the
    function stack. If you are not interested in filtering this particular
    instance of the device, you can just return STATUS_SUCCESS without creating
    a framework device.

Arguments:

    Driver - Handle to a framework driver object created in DriverEntry

    DeviceInit - Pointer to a framework-allocated WDFDEVICE_INIT structure.

Return Value:

    NTSTATUS

--*/
{
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    PFILTER_EXTENSION       filterExt;
    NTSTATUS                status;
    WDFDEVICE               device;
    ULONG                   serialNo;
    ULONG                   returnSize;

    PAGED_CODE ();

    UNREFERENCED_PARAMETER(Driver);

    //
    // Get some property of the device you are about to attach and check
    // to see if that's the one you are interested. For demonstration
    // we will get the UINumber of the device. The bus driver reports the
    // serial number as the UINumber.
    // 获取本设备的序号(serial number)，应该不是下层，就是马上要创建的device object的序号
    status = WdfFdoInitQueryProperty(DeviceInit,
                                  DevicePropertyUINumber,//枚举
                                  sizeof(serialNo),
                                  &serialNo, //输出
                                  &returnSize); //输出
    if(!NT_SUCCESS(status)){
        KdPrint(("Failed to get the property of PDO: 0x%p\n", DeviceInit));
    }

    //-----------------------------------------------------------
    // Tell the framework that you are filter driver. Framework
    // takes care of inherting all the device flags & characterstics
    // from the lower device you are attaching to.
    //-----------------------------------------------------------
    WdfFdoInitSetFilter(DeviceInit);//告诉frame本驱动是过滤驱动

    //
    // Specify the size of device extension where we track per device
    // context.
    //

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, FILTER_EXTENSION);

    //
    // We will just register for cleanup notification because we have to
    // delete the control-device when the last instance of the device goes
    // away. If we don't delete, the driver wouldn't get unloaded automatcially
    // by the PNP subsystem.
    //
    deviceAttributes.EvtCleanupCallback = FilterEvtDeviceContextCleanup;//因为我们要删除control device对象，非pnp能干得了

    //-----------------------------------------------------------
    // Create a framework device object.This call will inturn create
    // a WDM deviceobject, attach to the lower stack and set the
    // appropriate flags and attributes.
    //-----------------------------------------------------------
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);//返回来会创建WDM的设备在对象，然后attach
    if (!NT_SUCCESS(status)) {
        KdPrint( ("WdfDeviceCreate failed with status code 0x%x\n", status));
        return status;
    }

    filterExt = FilterGetData(device);
    filterExt->SerialNo = serialNo; //上面通过WdfFdoInitQueryProperty得到的

    //
    // Add this device to the FilterDevice collection.
    //
    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);
    //
    // WdfCollectionAdd takes a reference on the item object and removes
    // it when you call WdfCollectionRemove.
    //
    status = WdfCollectionAdd(FilterDeviceCollection, device/*刚刚创建的*/);
    if (!NT_SUCCESS(status)) {
        KdPrint( ("WdfCollectionAdd failed with status code 0x%x\n", status));
    }
    WdfWaitLockRelease(FilterDeviceCollectionLock);

    //-----------------------------------------------------------
    // 重要的地方：Create a control device
    //-----------------------------------------------------------
    status = FilterCreateControlDevice(device);
    if (!NT_SUCCESS(status)) {
        KdPrint( ("FilterCreateControlDevice failed with status 0x%x\n",
                                status));
        //
        // Let us not fail AddDevice just because we weren't able to create the
        // control device.
        //
        status = STATUS_SUCCESS;
    }

    return status;
}

#pragma warning(push)
#pragma warning(disable:28118) // this callback will run at IRQL=PASSIVE_LEVEL
_Use_decl_annotations_
VOID
FilterEvtDeviceContextCleanup(
    WDFOBJECT Device
    )
/*++

Routine Description:

   EvtDeviceRemove event callback must perform any operations that are
   necessary before the specified device is removed. The framework calls
   the driver's EvtDeviceRemove callback when the PnP manager sends
   an IRP_MN_REMOVE_DEVICE request to the driver stack.

Arguments:

    Device - Handle to a framework device object.

Return Value:

    WDF status code

--*/
{
    ULONG   count;

    PAGED_CODE();

    KdPrint(("Entered FilterEvtDeviceContextCleanup\n"));

    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);

    count = WdfCollectionGetCount(FilterDeviceCollection);//对象数量

    if(count == 1)
    {
         //
         // We are the last instance. So let us delete the control-device
         // so that driver can unload when the FilterDevice is deleted.
         // We absolutely have to do the deletion of control device with
         // the collection lock acquired because we implicitly use this
         // lock to protect ControlDevice global variable. We need to make
         // sure another thread doesn't attempt to create while we are
         // deleting the device.
         //
         FilterDeleteControlDevice((WDFDEVICE)Device);//本地函数，在下面，最多创建1个管理所有的device
     }

    WdfCollectionRemove(FilterDeviceCollection, Device);//移除到collection外

    WdfWaitLockRelease(FilterDeviceCollectionLock);
}
#pragma warning(pop) // enable 28118 again

//自己初始化PWDFDEVICE_INIT的例子
//创建控制设备（control deviceobject）的例子
_Use_decl_annotations_
NTSTATUS
FilterCreateControlDevice(
    WDFDEVICE Device
    )
/*++

Routine Description:

    This routine is called to create a control deviceobject so that application
    can talk to the filter driver directly instead of going through the entire
    device stack. This kind of control device object is useful if the filter
    driver is underneath another driver which prevents ioctls not known to it
    or if the driver's dispatch routine is owned by some other (port/class)
    driver and it doesn't allow any custom ioctls.

    NOTE: Since the control device is global to the driver and accessible to
    all instances of the device this filter is attached to, we create only once
    when the first instance of the device is started and delete it when the
    last instance gets removed.

Arguments:

    Device - Handle to a filter device object.

Return Value:

    WDF status code

--*/
{
    PWDFDEVICE_INIT             pInit = NULL;
    WDFDEVICE                   controlDevice = NULL;
    WDF_OBJECT_ATTRIBUTES       controlAttributes;
    WDF_IO_QUEUE_CONFIG         ioQueueConfig;
    BOOLEAN                     bCreate = FALSE;
    NTSTATUS                    status;
    WDFQUEUE                    queue;
    DECLARE_CONST_UNICODE_STRING(ntDeviceName, NTDEVICE_NAME_STRING) ;
    DECLARE_CONST_UNICODE_STRING(symbolicLinkName, SYMBOLIC_NAME_STRING) ;

    PAGED_CODE();

    //
    // First find out whether any ControlDevice has been created. If the
    // collection has more than one device then we know somebody has already
    // created or in the process of creating the device.
    //
    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);

    if(WdfCollectionGetCount(FilterDeviceCollection) == 1) {
        bCreate = TRUE;
    }

    WdfWaitLockRelease(FilterDeviceCollectionLock);

    if(!bCreate) {
        //
        // Control device is already created. So return success.
        //
        return STATUS_SUCCESS;
    }

    KdPrint(("Creating Control Device\n"));

	//----------------------------------------------------------------
	//	创建controlDevice
	//----------------------------------------------------------------
    
    //
    // In order to create a control device, we first need to allocate a
    // WDFDEVICE_INIT structure and set all properties.
    //
	//pInit初始化之1/3：分配内存，指定安全权限
    pInit = WdfControlDeviceInitAllocate(
                            WdfDeviceGetDriver(Device),
                            &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R
                            );

    if (pInit == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Error;
    }

    //----------------------------------------------------------------
    // Set exclusive to false so that more than one app can talk to the
    // control device simultaneously.
    //----------------------------------------------------------------
	//pInit初始化之2/3：设置共享
    WdfDeviceInitSetExclusive(pInit, FALSE);

	//pInit初始化之3/3：指定设备名
    status = WdfDeviceInitAssignName(pInit, &ntDeviceName); //L"\\Device\\ToasterFilter"

    if (!NT_SUCCESS(status)) {
        goto Error;
    }

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&controlAttributes,
                                    CONTROL_DEVICE_EXTENSION); // Specify the size of device context
    status = WdfDeviceCreate(&pInit,
                             &controlAttributes,
                             &controlDevice);//输出，问题是controlDevice贴在哪儿？
    if (!NT_SUCCESS(status)) {
        goto Error;
    }


	//----------------------------------------------------------------
	//	创建SymbolicLink
	//----------------------------------------------------------------

    //----------------------------------------------------------------
    // Create a symbolic link for the control object so that usermode can open
    // the device.
    //----------------------------------------------------------------
	//问题：创建的symbolicLink放在哪里了？
    status = WdfDeviceCreateSymbolicLink(controlDevice,//输入
                                &symbolicLinkName);//输入，L"\\DosDevices\\ToasterFilter"

    if (!NT_SUCCESS(status)) {
        goto Error;
    }

	//----------------------------------------------------------------
	//	创建queue
	//----------------------------------------------------------------

    //
    // Configure the default queue associated with the control device object
    // to be Serial so that request passed to EvtIoDeviceControl are serialized.
    //

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig,
                             WdfIoQueueDispatchSequential);

    ioQueueConfig.EvtIoDeviceControl = FilterEvtIoDeviceControl;

    //----------------------------------------------------------------
    // Framework by default creates non-power managed queues for
    // filter drivers.
    //----------------------------------------------------------------
    status = WdfIoQueueCreate(controlDevice,
                                &ioQueueConfig,
                                WDF_NO_OBJECT_ATTRIBUTES,
                                &queue // pointer to default queue
                                );
    if (!NT_SUCCESS(status)) {
        goto Error;
    }


	//----------------------------------------------------------------
	//	启用controlDevice
	//----------------------------------------------------------------

    //----------------------------------------------------------------
    // Control devices must notify WDF when they are done initializing.   I/O is
    // rejected until this call is made.
    //----------------------------------------------------------------
    WdfControlFinishInitializing(controlDevice);

    ControlDevice = controlDevice;

    return STATUS_SUCCESS;

Error:

    if (pInit != NULL) {
        WdfDeviceInitFree(pInit);
    }

    if (controlDevice != NULL) {
        //
        // Release the reference on the newly created object, since
        // we couldn't initialize it.
        //
        WdfObjectDelete(controlDevice);
        controlDevice = NULL;
    }

    return status;
}

_Use_decl_annotations_
VOID
FilterDeleteControlDevice(
    WDFDEVICE Device
    )
/*++

Routine Description:

    This routine deletes the control by doing a simple dereference.

Arguments:

    Device - Handle to a framework filter device object.

Return Value:

    WDF status code

--*/
{
    UNREFERENCED_PARAMETER(Device);

    PAGED_CODE();

    KdPrint(("Deleting Control Device\n"));

    if (ControlDevice) {
        WdfObjectDelete(ControlDevice);
        ControlDevice = NULL;
    }
}

//该控制设备（control deviceobject）例子只有这么个实际功能演示
//ioQueueConfig.EvtIoDeviceControl = FilterEvtIoDeviceControl;
#pragma warning(push)
#pragma warning(disable:28118) // this callback will run at IRQL=PASSIVE_LEVEL
_Use_decl_annotations_
VOID
FilterEvtIoDeviceControl(
    WDFQUEUE         Queue,
    WDFREQUEST       Request,
    size_t           OutputBufferLength,
    size_t           InputBufferLength,
    ULONG            IoControlCode
    )
/*++
Routine Description:

    This event is called when the framework receives IRP_MJ_DEVICE_CONTROL
    requests from the system.

Arguments:

    Queue - Handle to the framework queue object that is associated
            with the I/O request.
    Request - Handle to a framework request object.

    OutputBufferLength - length of the request's output buffer,
                        if an output buffer is available.
    InputBufferLength - length of the request's input buffer,
                        if an input buffer is available.

    IoControlCode - the driver-defined or system-defined I/O control code
                    (IOCTL) that is associated with the request.

Return Value:

   VOID

--*/
{
    ULONG               i;
    ULONG               noItems;
    WDFDEVICE           hFilterDevice;
    PFILTER_EXTENSION   filterExt;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(IoControlCode);

    PAGED_CODE();

    KdPrint(("Ioctl recieved into filter control object.\n"));

    WdfWaitLockAcquire(FilterDeviceCollectionLock, NULL);

    noItems = WdfCollectionGetCount(FilterDeviceCollection);

    for(i=0; i<noItems ; i++) {

        hFilterDevice = WdfCollectionGetItem(FilterDeviceCollection, i);

        filterExt = FilterGetData(hFilterDevice);

        KdPrint(("Serial No: %d\n", filterExt->SerialNo));
    }

    WdfWaitLockRelease(FilterDeviceCollectionLock);

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, 0);
}
#pragma warning(pop) // enable 28118 again
