/*
 * Copyright 2010-2013, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */


#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include <device_manager.h>
#include <Drivers.h>

#include <AutoDeleter.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>

#include <fs/KPath.h>
#include <lock.h>
#include <vm/vm.h>
#include <vm/VMCache.h>
#include <vm/vm_page.h>

#include "dma_resources.h"
#include "io_requests.h"
#include "IOSchedulerSimple.h"


//#define TRACE_CHECK_SUM_DEVICE
#ifdef TRACE_CHECK_SUM_DEVICE
#	define TRACE(x...)	dprintf(x)
#else
#	define TRACE(x) do {} while (false)
#endif


// parameters for the DMA resource
static const uint32 kDMAResourceBufferCount			= 16;
static const uint32 kDMAResourceBounceBufferCount	= 16;

static const char* const kDriverModuleName
	= "drivers/disk/virtual/ram_disk/driver_v1";
static const char* const kControlDeviceModuleName
	= "drivers/disk/virtual/ram_disk/control/device_v1";
static const char* const kRawDeviceModuleName
	= "drivers/disk/virtual/ram_disk/raw/device_v1";

static const char* const kControlDeviceName
	= "disk/virtual/ram/control";
static const char* const kRawDeviceBaseName = "disk/virtual/ram";

static const char* const kFilePathItem = "ram_disk/file_path";
static const char* const kDeviceSizeItem = "ram_disk/device_size";


struct RawDevice;
typedef DoublyLinkedList<RawDevice> RawDeviceList;

struct device_manager_info* sDeviceManager;

static RawDeviceList sDeviceList;
static mutex sDeviceListLock = MUTEX_INITIALIZER("ram disk device list");


struct Device {
	Device(device_node* node)
		:
		fNode(node)
	{
		mutex_init(&fLock, "ram disk device");
	}

	virtual ~Device()
	{
		mutex_destroy(&fLock);
	}

	bool Lock()		{ mutex_lock(&fLock); return true; }
	void Unlock()	{ mutex_unlock(&fLock); }

	device_node* Node() const	{ return fNode; }

	virtual status_t PublishDevice() = 0;

protected:
	mutex			fLock;
	device_node*	fNode;
};


struct ControlDevice : Device {
	ControlDevice(device_node* node)
		:
		Device(node)
	{
	}

	status_t Register(const char* fileName, uint64 deviceSize)
	{
		device_attr attrs[] = {
			{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
				{string: "RAM Disk Raw Device"}},
			{kFilePathItem, B_STRING_TYPE, {string: fileName}},
			{kDeviceSizeItem, B_UINT64_TYPE, {ui64: deviceSize}},
			{NULL}
		};

		return sDeviceManager->register_node(
			sDeviceManager->get_parent_node(Node()), kDriverModuleName, attrs,
			NULL, NULL);
	}

	virtual status_t PublishDevice()
	{
		return sDeviceManager->publish_device(Node(), kControlDeviceName,
			kControlDeviceModuleName);
	}
};


struct RawDevice : Device, DoublyLinkedListLinkImpl<RawDevice> {
	RawDevice(device_node* node)
		:
		Device(node),
		fIndex(-1),
		fDeviceSize(0),
		fDeviceName(NULL),
		fCache(NULL),
		fDMAResource(NULL),
		fIOScheduler(NULL)
	{
	}

	virtual ~RawDevice()
	{
		if (fIndex >= 0) {
			MutexLocker locker(sDeviceListLock);
			sDeviceList.Remove(this);
		}

		free(fDeviceName);
	}

	int32 Index() const				{ return fIndex; }
	off_t DeviceSize() const		{ return fDeviceSize; }
	const char* DeviceName() const	{ return fDeviceName; }

	status_t Init(uint64 deviceSize)
	{
		fDeviceSize = (deviceSize + B_PAGE_SIZE - 1) / B_PAGE_SIZE
			* B_PAGE_SIZE;

		if (fDeviceSize < B_PAGE_SIZE
			|| (uint64)fDeviceSize / B_PAGE_SIZE
				> vm_page_num_pages() * 2 / 3) {
			return B_BAD_VALUE;
		}

		// find a free slot
		fIndex = 0;
		RawDevice* nextDevice = NULL;
		MutexLocker locker(sDeviceListLock);
		for (RawDeviceList::Iterator it = sDeviceList.GetIterator();
				(nextDevice = it.Next()) != NULL;) {
			if (nextDevice->Index() > fIndex)
				break;
			fIndex = nextDevice->Index() + 1;
		}

		sDeviceList.InsertBefore(nextDevice, this);

		// construct our device path
		KPath path(kRawDeviceBaseName);
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%" B_PRId32 "/raw", fIndex);

		status_t error = path.Append(buffer);
		if (error != B_OK)
			return error;

		fDeviceName = path.DetachBuffer();

		return B_OK;
	}

	status_t Prepare()
	{
		status_t error = VMCacheFactory::CreateAnonymousCache(fCache, false, 0,
			0, false, VM_PRIORITY_SYSTEM);
		if (error != B_OK) {
			Unprepare();
			return error;
		}

		fCache->temporary = 1;
		fCache->virtual_end = fDeviceSize;

		error = fCache->Commit(fDeviceSize, VM_PRIORITY_SYSTEM);
		if (error != B_OK) {
			Unprepare();
			return error;
		}

		// no DMA restrictions
		const dma_restrictions restrictions = {};

		fDMAResource = new(std::nothrow) DMAResource;
		if (fDMAResource == NULL) {
			Unprepare();
			return B_NO_MEMORY;
		}

		error = fDMAResource->Init(restrictions, B_PAGE_SIZE,
			kDMAResourceBufferCount, kDMAResourceBounceBufferCount);
		if (error != B_OK) {
			Unprepare();
			return error;
		}

		fIOScheduler = new(std::nothrow) IOSchedulerSimple(fDMAResource);
		if (fIOScheduler == NULL) {
			Unprepare();
			return B_NO_MEMORY;
		}

		error = fIOScheduler->Init("ram disk device scheduler");
		if (error != B_OK) {
			Unprepare();
			return error;
		}

		fIOScheduler->SetCallback(&_DoIOEntry, this);

		return B_OK;
	}

	void Unprepare()
	{
		delete fIOScheduler;
		fIOScheduler = NULL;

		delete fDMAResource;
		fDMAResource = NULL;

		if (fCache != NULL) {
			fCache->Lock();
			fCache->ReleaseRefAndUnlock();
			fCache = NULL;
		}
	}

	status_t DoIO(IORequest* request)
	{
		return fIOScheduler->ScheduleRequest(request);
	}

	virtual status_t PublishDevice()
	{
		return sDeviceManager->publish_device(Node(), fDeviceName,
			kRawDeviceModuleName);
	}

private:
	static status_t _DoIOEntry(void* data, IOOperation* operation)
	{
		return ((RawDevice*)data)->_DoIO(operation);
	}

	status_t _DoIO(IOOperation* operation)
	{
		off_t offset = operation->Offset();
		generic_size_t length = operation->Length();

		ASSERT(offset % B_PAGE_SIZE == 0);
		ASSERT(length % B_PAGE_SIZE == 0);

		const generic_io_vec* vecs = operation->Vecs();
		generic_size_t vecOffset = 0;
		bool isWrite = operation->IsWrite();

		vm_page** pages = new(std::nothrow) vm_page*[length / B_PAGE_SIZE];
		if (pages == NULL)
			return B_NO_MEMORY;
		ArrayDeleter<vm_page*> pagesDeleter(pages);

		_GetPages(offset, length, isWrite, pages);

		status_t error = B_OK;
		size_t index = 0;

		while (length > 0) {
			vm_page* page = pages[index];

			error = _CopyData(page, vecs, vecOffset, isWrite);
			if (error != B_OK)
				break;

			offset += B_PAGE_SIZE;
			length -= B_PAGE_SIZE;
			index++;
		}

		_PutPages(operation->Offset(), operation->Length(), pages,
			error == B_OK);

		if (error != B_OK) {
			fIOScheduler->OperationCompleted(operation, error, 0);
			return error;
		}

		fIOScheduler->OperationCompleted(operation, B_OK, operation->Length());
		return B_OK;
	}

	void _GetPages(off_t offset, off_t length, bool isWrite, vm_page** pages)
	{
		// get the pages, we already have
		AutoLocker<VMCache> locker(fCache);

		size_t pageCount = length / B_PAGE_SIZE;
		size_t index = 0;
		size_t missingPages = 0;

		while (length > 0) {
			vm_page* page = fCache->LookupPage(offset);
			if (page != NULL) {
				if (page->busy) {
					fCache->WaitForPageEvents(page, PAGE_EVENT_NOT_BUSY, true);
					continue;
				}

				page->busy = true;
			} else
				missingPages++;

			pages[index++] = page;
			offset += B_PAGE_SIZE;
			length -= B_PAGE_SIZE;
		}

		locker.Unlock();

		// For a write we need to reserve the missing pages.
		if (isWrite && missingPages > 0) {
			vm_page_reservation reservation;
			vm_page_reserve_pages(&reservation, missingPages,
				VM_PRIORITY_SYSTEM);

			for (size_t i = 0; i < pageCount; i++) {
				if (pages[i] != NULL)
					continue;

				pages[i] = vm_page_allocate_page(&reservation,
					PAGE_STATE_WIRED | VM_PAGE_ALLOC_BUSY);

				if (--missingPages == 0)
					break;
			}

			vm_page_unreserve_pages(&reservation);
		}
	}

	void _PutPages(off_t offset, off_t length, vm_page** pages, bool success)
	{
		AutoLocker<VMCache> locker(fCache);

		// Mark all pages unbusy. On error free the newly allocated pages.
		size_t index = 0;

		while (length > 0) {
			vm_page* page = pages[index++];
			if (page != NULL) {
				if (page->CacheRef() == NULL) {
					if (success) {
						fCache->InsertPage(page, offset);
						fCache->MarkPageUnbusy(page);
					} else
						vm_page_free(NULL, page);
				} else
					fCache->MarkPageUnbusy(page);
			}

			offset += B_PAGE_SIZE;
			length -= B_PAGE_SIZE;
		}
	}

	status_t _CopyData(vm_page* page, const generic_io_vec*& vecs,
		generic_size_t& vecOffset, bool toPage)
	{
		// map page to virtual memory
		Thread* thread = thread_get_current_thread();
		uint8* pageData = NULL;
		void* handle;
		if (page != NULL) {
			thread_pin_to_current_cpu(thread);
			addr_t virtualAddress;
			status_t error = vm_get_physical_page_current_cpu(
				page->physical_page_number * B_PAGE_SIZE, &virtualAddress,
				&handle);
			if (error != B_OK) {
				thread_unpin_from_current_cpu(thread);
				return error;
			}

			pageData = (uint8*)virtualAddress;
		}

		size_t length = B_PAGE_SIZE;
		while (length > 0) {
			size_t toCopy = std::min((generic_size_t)length,
				vecs->length - vecOffset);

			if (toCopy == 0) {
				vecs++;
				vecOffset = 0;
				continue;
			}

			phys_addr_t vecAddress = vecs->base + vecOffset;

			status_t error = toPage
				? vm_memcpy_from_physical(pageData, vecAddress, toCopy, false)
				: (page != NULL
					? vm_memcpy_to_physical(vecAddress, pageData, toCopy, false)
					: vm_memset_physical(vecAddress, 0, toCopy));
			if (error != B_OK)
				return error;

			pageData += toCopy;
			length -= toCopy;
			vecOffset += toCopy;
		}

		if (page != NULL) {
			vm_put_physical_page_current_cpu((addr_t)pageData, handle);
			thread_unpin_from_current_cpu(thread);
		}

		return B_OK;
	}

private:
	int32			fIndex;
	off_t			fDeviceSize;
	char*			fDeviceName;
	VMCache*		fCache;
	DMAResource*	fDMAResource;
	IOScheduler*	fIOScheduler;
};


struct RawDeviceCookie {
	RawDeviceCookie(RawDevice* device, int openMode)
		:
		fDevice(device),
		fOpenMode(openMode)
	{
	}

	RawDevice* Device() const	{ return fDevice; }
	int OpenMode() const		{ return fOpenMode; }

private:
	RawDevice*	fDevice;
	int			fOpenMode;
};


// #pragma mark -


static bool
parse_command_line(char* buffer, char**& _argv, int& _argc)
{
	// Process the argument string. We split at whitespace, heeding quotes and
	// escaped characters. The processed arguments are written to the given
	// buffer, separated by single null chars.
	char* start = buffer;
	char* out = buffer;
	bool pendingArgument = false;
	int argc = 0;
	while (*start != '\0') {
		// ignore whitespace
		if (isspace(*start)) {
			if (pendingArgument) {
				*out = '\0';
				out++;
				argc++;
				pendingArgument = false;
			}
			start++;
			continue;
		}

		pendingArgument = true;

		if (*start == '"' || *start == '\'') {
			// quoted text -- continue until closing quote
			char quote = *start;
			start++;
			while (*start != '\0' && *start != quote) {
				if (*start == '\\' && quote == '"') {
					start++;
					if (*start == '\0')
						break;
				}
				*out = *start;
				start++;
				out++;
			}

			if (*start != '\0')
				start++;
		} else {
			// unquoted text
			if (*start == '\\') {
				// escaped char
				start++;
				if (start == '\0')
					break;
			}

			*out = *start;
			start++;
			out++;
		}
	}

	if (pendingArgument) {
		*out = '\0';
		argc++;
	}

	// allocate argument vector
	char** argv = new(std::nothrow) char*[argc + 1];
	if (argv == NULL)
		return false;

	// fill vector
	start = buffer;
	for (int i = 0; i < argc; i++) {
		argv[i] = start;
		start += strlen(start) + 1;
	}
	argv[argc] = NULL;

	_argv = argv;
	_argc = argc;
	return true;
}


//	#pragma mark - driver


static float
ram_disk_driver_supports_device(device_node* parent)
{
	const char* bus = NULL;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			== B_OK
		&& strcmp(bus, "generic") == 0) {
		return 0.8;
	}

	return -1;
}


static status_t
ram_disk_driver_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{string: "RAM Disk Control Device"}},
		{NULL}
	};

	return sDeviceManager->register_node(parent, kDriverModuleName, attrs, NULL,
		NULL);
}


static status_t
ram_disk_driver_init_driver(device_node* node, void** _driverCookie)
{
//	const char* fileName;
	uint64 deviceSize;
// 	if (sDeviceManager->get_attr_string(node, kFilePathItem, &fileName, false)
// 			== B_OK) {
	if (sDeviceManager->get_attr_uint64(node, kDeviceSizeItem, &deviceSize,
			false) == B_OK) {
		RawDevice* device = new(std::nothrow) RawDevice(node);
		if (device == NULL)
			return B_NO_MEMORY;

//		status_t error = device->Init(fileName);
		status_t error = device->Init(deviceSize);
		if (error != B_OK) {
			delete device;
			return error;
		}

		*_driverCookie = (Device*)device;
	} else {
		ControlDevice* device = new(std::nothrow) ControlDevice(node);
		if (device == NULL)
			return B_NO_MEMORY;

		*_driverCookie = (Device*)device;
	}

	return B_OK;
}


static void
ram_disk_driver_uninit_driver(void* driverCookie)
{
	Device* device = (Device*)driverCookie;
	delete device;
}


static status_t
ram_disk_driver_register_child_devices(void* driverCookie)
{
	Device* device = (Device*)driverCookie;
	return device->PublishDevice();
}


//	#pragma mark - control device


static status_t
ram_disk_control_device_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
ram_disk_control_device_uninit_device(void* deviceCookie)
{
}


static status_t
ram_disk_control_device_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	*_cookie = deviceCookie;
	return B_OK;
}


static status_t
ram_disk_control_device_close(void* cookie)
{
	return B_OK;
}


static status_t
ram_disk_control_device_free(void* cookie)
{
	return B_OK;
}


static status_t
ram_disk_control_device_read(void* cookie, off_t position, void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_OK;
}


static status_t
ram_disk_control_device_write(void* cookie, off_t position, const void* data,
	size_t* _length)
{
	ControlDevice* device = (ControlDevice*)cookie;

	if (position != 0)
		return B_BAD_VALUE;

	// copy data to stack buffer
	char* buffer = (char*)malloc(*_length + 1);
	if (buffer == NULL)
		return B_NO_MEMORY;
	MemoryDeleter bufferDeleter(buffer);

	if (IS_USER_ADDRESS(data)) {
		if (user_memcpy(buffer, data, *_length) != B_OK)
			return B_BAD_ADDRESS;
	} else
		memcpy(buffer, data, *_length);

	buffer[*_length] = '\0';

	// parse arguments
	char** argv;
	int argc;
	if (!parse_command_line(buffer, argv, argc))
		return B_NO_MEMORY;
	ArrayDeleter<char*> argvDeleter(argv);

	if (argc == 0) {
		dprintf("\"help\" for usage!\n");
		return B_BAD_VALUE;
	}

	// execute command
	if (strcmp(argv[0], "help") == 0) {
		// help
// 		dprintf("register <path>\n");
// 		dprintf("  Registers file <path> as a new ram disk device.\n");
		dprintf("register <size>\n");
		dprintf("  Registers a new ram disk device with size <size>.\n");
		dprintf("unregister <device>\n");
		dprintf("  Unregisters <device>.\n");
	} else if (strcmp(argv[0], "register") == 0) {
		// register
		if (argc != 2) {
			dprintf("Usage: register <size>\n");
			return B_BAD_VALUE;
		}

		// parse size argument
		char* end;
		uint64 deviceSize = strtoll(argv[1], &end, 0);
		if (end == argv[1]) {
			dprintf("Invalid size argument: \"%s\"\n", argv[1]);
			return B_BAD_VALUE;
		}

		switch (*end) {
			case 'g':
				deviceSize *= 1024;
			case 'm':
				deviceSize *= 1024;
			case 'k':
				deviceSize *= 1024;
				break;
		}

		return device->Register(NULL, deviceSize);
	} else if (strcmp(argv[0], "unregister") == 0) {
		// unregister
		if (argc != 2) {
			dprintf("Usage: unregister <device>\n");
			return B_BAD_VALUE;
		}

		const char* deviceName = argv[1];
		if (strncmp(deviceName, "/dev/", 5) == 0)
			deviceName += 5;

		// find the device in the list and unregister it
		MutexLocker locker(sDeviceListLock);
		for (RawDeviceList::Iterator it = sDeviceList.GetIterator();
				RawDevice* device = it.Next();) {
			if (strcmp(device->DeviceName(), deviceName) == 0) {
				// TODO: Race condition: We should mark the device as going to
				// be unregistered, so no one else can try the same after we
				// unlock!
				locker.Unlock();
// TODO: The following doesn't work! unpublish_device(), as per implementation
// (partially commented out) and unregister_node() returns B_BUSY.
				status_t error = sDeviceManager->unpublish_device(
					device->Node(), device->DeviceName());
				if (error != B_OK) {
					dprintf("Failed to unpublish device \"%s\": %s\n",
						deviceName, strerror(error));
					return error;
				}

				error = sDeviceManager->unregister_node(device->Node());
				if (error != B_OK) {
					dprintf("Failed to unregister node \"%s\": %s\n",
						deviceName, strerror(error));
					return error;
				}

				return B_OK;
			}
		}

		dprintf("Device \"%s\" not found!\n", deviceName);
		return B_BAD_VALUE;
	} else {
		dprintf("Invalid command \"%s\"!\n", argv[0]);
		return B_BAD_VALUE;
	}

	return B_OK;
}


static status_t
ram_disk_control_device_control(void* cookie, uint32 op, void* buffer,
	size_t length)
{
	return B_BAD_VALUE;
}


//	#pragma mark - raw device


static status_t
ram_disk_raw_device_init_device(void* driverCookie, void** _deviceCookie)
{
	RawDevice* device = static_cast<RawDevice*>((Device*)driverCookie);

	status_t error = device->Prepare();
	if (error != B_OK)
		return error;

	*_deviceCookie = device;
	return B_OK;
}


static void
ram_disk_raw_device_uninit_device(void* deviceCookie)
{
	RawDevice* device = (RawDevice*)deviceCookie;
	device->Unprepare();
}


static status_t
ram_disk_raw_device_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	RawDevice* device = (RawDevice*)deviceCookie;

	RawDeviceCookie* cookie = new(std::nothrow) RawDeviceCookie(device,
		openMode);
	if (cookie == NULL)
		return B_NO_MEMORY;

	*_cookie = cookie;
	return B_OK;
}


static status_t
ram_disk_raw_device_close(void* cookie)
{
	return B_OK;
}


static status_t
ram_disk_raw_device_free(void* _cookie)
{
	RawDeviceCookie* cookie = (RawDeviceCookie*)_cookie;
	delete cookie;
	return B_OK;
}


static status_t
ram_disk_raw_device_read(void* _cookie, off_t pos, void* buffer,
	size_t* _length)
{
	RawDeviceCookie* cookie = (RawDeviceCookie*)_cookie;
	RawDevice* device = cookie->Device();

	size_t length = *_length;

	if (pos >= device->DeviceSize())
		return B_BAD_VALUE;
	if (pos + length > device->DeviceSize())
		length = device->DeviceSize() - pos;

	IORequest request;
	status_t status = request.Init(pos, (addr_t)buffer, length, false, 0);
	if (status != B_OK)
		return status;

	status = device->DoIO(&request);
	if (status != B_OK)
		return status;

	status = request.Wait(0, 0);
	if (status == B_OK)
		*_length = length;
	return status;
}


static status_t
ram_disk_raw_device_write(void* _cookie, off_t pos, const void* buffer,
	size_t* _length)
{
	RawDeviceCookie* cookie = (RawDeviceCookie*)_cookie;
	RawDevice* device = cookie->Device();

	size_t length = *_length;

	if (pos >= device->DeviceSize())
		return B_BAD_VALUE;
	if (pos + length > device->DeviceSize())
		length = device->DeviceSize() - pos;

	IORequest request;
	status_t status = request.Init(pos, (addr_t)buffer, length, true, 0);
	if (status != B_OK)
		return status;

	status = device->DoIO(&request);
	if (status != B_OK)
		return status;

	status = request.Wait(0, 0);
	if (status == B_OK)
		*_length = length;

	return status;
}


static status_t
ram_disk_raw_device_io(void* _cookie, io_request* request)
{
	RawDeviceCookie* cookie = (RawDeviceCookie*)_cookie;
	RawDevice* device = cookie->Device();

	return device->DoIO(request);
}


static status_t
ram_disk_raw_device_control(void* _cookie, uint32 op, void* buffer,
	size_t length)
{
	RawDeviceCookie* cookie = (RawDeviceCookie*)_cookie;
	RawDevice* device = cookie->Device();

	switch (op) {
		case B_GET_DEVICE_SIZE:
		{
			size_t size = device->DeviceSize();
			return user_memcpy(buffer, &size, sizeof(size_t));
		}

		case B_SET_NONBLOCKING_IO:
		case B_SET_BLOCKING_IO:
			return B_OK;

		case B_GET_READ_STATUS:
		case B_GET_WRITE_STATUS:
		{
			bool value = true;
			return user_memcpy(buffer, &value, sizeof(bool));
		}

		case B_GET_GEOMETRY:
		case B_GET_BIOS_GEOMETRY:
		{
			device_geometry geometry;
			geometry.bytes_per_sector = B_PAGE_SIZE;
			geometry.sectors_per_track = 1;
			geometry.cylinder_count = device->DeviceSize() / B_PAGE_SIZE;
				// TODO: We're limited to 2^32 * B_PAGE_SIZE, if we don't use
				// sectors_per_track and head_count.
			geometry.head_count = 1;
			geometry.device_type = B_DISK;
			geometry.removable = true;
			geometry.read_only = false;
			geometry.write_once = false;

			return user_memcpy(buffer, &geometry, sizeof(device_geometry));
		}

		case B_GET_MEDIA_STATUS:
		{
			status_t status = B_OK;
			return user_memcpy(buffer, &status, sizeof(status_t));
		}

		case B_SET_UNINTERRUPTABLE_IO:
		case B_SET_INTERRUPTABLE_IO:
		case B_FLUSH_DRIVE_CACHE:
			return B_OK;
	}

	return B_BAD_VALUE;
}


// #pragma mark -


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};


static const struct driver_module_info sChecksumDeviceDriverModule = {
	{
		kDriverModuleName,
		0,
		NULL
	},

	ram_disk_driver_supports_device,
	ram_disk_driver_register_device,
	ram_disk_driver_init_driver,
	ram_disk_driver_uninit_driver,
	ram_disk_driver_register_child_devices
};

static const struct device_module_info sChecksumControlDeviceModule = {
	{
		kControlDeviceModuleName,
		0,
		NULL
	},

	ram_disk_control_device_init_device,
	ram_disk_control_device_uninit_device,
	NULL,

	ram_disk_control_device_open,
	ram_disk_control_device_close,
	ram_disk_control_device_free,

	ram_disk_control_device_read,
	ram_disk_control_device_write,
	NULL,	// io

	ram_disk_control_device_control,

	NULL,	// select
	NULL	// deselect
};

static const struct device_module_info sChecksumRawDeviceModule = {
	{
		kRawDeviceModuleName,
		0,
		NULL
	},

	ram_disk_raw_device_init_device,
	ram_disk_raw_device_uninit_device,
	NULL,

	ram_disk_raw_device_open,
	ram_disk_raw_device_close,
	ram_disk_raw_device_free,

	ram_disk_raw_device_read,
	ram_disk_raw_device_write,
	ram_disk_raw_device_io,

	ram_disk_raw_device_control,

	NULL,	// select
	NULL	// deselect
};

const module_info* modules[] = {
	(module_info*)&sChecksumDeviceDriverModule,
	(module_info*)&sChecksumControlDeviceModule,
	(module_info*)&sChecksumRawDeviceModule,
	NULL
};