// Copyright (c) 2015-2017, Satoshi Tanda. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements an entry point of the driver.

#ifndef POOL_NX_OPTIN
#define POOL_NX_OPTIN 1
#endif
#include "driver.h"
#include "common.h"
#include "global_object.h"
#include "hotplug_callback.h"
#include "log.h"
#include "power_callback.h"
#include "util.h"
#include "vm.h"
#include "performance.h"

//注释版本，为了能够完全读懂，把与“传统国内内内核驱动发的习惯”不同的地方都作了注释，也希望大家能够开发出更加规范的内核驱动
extern "C" {
////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

////////////////////////////////////////////////////////////////////////////////
//
// types
//

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

 //see https://docs.microsoft.com/en-us/windows-hardware/drivers/ddi/content/wdm/nc-wdm-driver_initialize
DRIVER_INITIALIZE DriverEntry;

 //see https://msdn.microsoft.com/en-us/library/windows/hardware/ff564886(v=vs.85).aspx
static DRIVER_UNLOAD DriverpDriverUnload;
 //_IRQL_requires_max_(irql),irql 是可调用函数的最大 IRQL
 //see https://msdn.microsoft.com/zh-cn/library/hh454226
_IRQL_requires_max_(PASSIVE_LEVEL) bool DriverpIsSuppoetedOS();

 //#pragma alloc_text是使编译器把代码放到特定段的宏
 //see https://blog.csdn.net/wolfman125/article/details/56665300
#if defined(ALLOC_PRAGMA)
//将以下代码放到INIT段
#pragma alloc_text(INIT, DriverEntry)
//将以下代码放到分页内存中
#pragma alloc_text(PAGE, DriverpDriverUnload)
//将以下代码放到INIT段
#pragma alloc_text(INIT, DriverpIsSuppoetedOS)
#endif

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

// 驱动程序入口点
//_Use_decl_annotations_ 可能用于在函数定义 （也称为函数体） 上替代的标头中的批注列表。 当_Use_decl_annotations_是相同的函数的范围内标头显示的批注使用像它们也会存在于具有定义_Use_decl_annotations_批注。
//see https://docs.microsoft.com/zh-cn/visualstudio/code-quality/annotating-function-behavior?view=vs-2015
_Use_decl_annotations_ NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                            PUNICODE_STRING registry_path) {
  UNREFERENCED_PARAMETER(registry_path);

  //是 PAGED_ASSERT(KeGetCurrentIrql() <= APC_LEVEL);
  PAGED_CODE();

  static const wchar_t kLogFilePath[] = L"\\SystemRoot\\HyperPlatform.log";
  static const auto kLogLevel =
      (IsReleaseBuild()) ? kLogPutLevelInfo | kLogOptDisableFunctionName
                         : kLogPutLevelDebug | kLogOptDisableFunctionName;

  auto status = STATUS_UNSUCCESSFUL;
  driver_object->DriverUnload = DriverpDriverUnload;
  //等价于KdBreakPoint()
  HYPERPLATFORM_COMMON_DBG_BREAK();

  // Request NX Non-Paged Pool when available
  ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

  // Initialize log functions
  bool need_reinitialization = false;
  // 打开log记录
  status = LogInitialization(kLogLevel, kLogFilePath);
  if (status == STATUS_REINITIALIZATION_NEEDED) {
    need_reinitialization = true;
  } else if (!NT_SUCCESS(status)) {
    return status;
  }

  // Test if the system is supported
  // 判断系统是否支持
  if (!DriverpIsSuppoetedOS()) {
    LogTermination();
    return STATUS_CANCELLED;
  }

  // Initialize global variables
  // 初始化全局变量
  status = GlobalObjectInitialization();
  if (!NT_SUCCESS(status)) {
    LogTermination();
    return status;
  }

  // Initialize perf functions
  // 初始化 性能 函数
  status = PerfInitialization();
  if (!NT_SUCCESS(status)) {
    GlobalObjectTermination();
    LogTermination();
    return status;
  }

  // Initialize utility functions
  // 初始化 utility 函数
  status = UtilInitialization(driver_object);
  if (!NT_SUCCESS(status)) {
    PerfTermination();
    GlobalObjectTermination();
    LogTermination();
    return status;
  }

  // Initialize power callback
  // 初始化 电源 回调
  status = PowerCallbackInitialization();
  if (!NT_SUCCESS(status)) {
    UtilTermination();
    PerfTermination();
    GlobalObjectTermination();
    LogTermination();
    return status;
  }

  // Initialize hot-plug callback
  // 初始化 热插件 回调
  status = HotplugCallbackInitialization();
  if (!NT_SUCCESS(status)) {
    PowerCallbackTermination();
    UtilTermination();
    PerfTermination();
    GlobalObjectTermination();
    LogTermination();
    return status;
  }

  // Virtualize all processors
  // 虚拟化 所有处理器
  status = VmInitialization();
  if (!NT_SUCCESS(status)) {
    HotplugCallbackTermination();
    PowerCallbackTermination();
    UtilTermination();
    PerfTermination();
    GlobalObjectTermination();
    LogTermination();
    return status;
  }

  // Register re-initialization for the log functions if needed
  if (need_reinitialization) {
    LogRegisterReinitialization(driver_object);
  }

  HYPERPLATFORM_LOG_INFO("The VMM has been installed.");
  return status;
}

// 卸载相关例程
// Unload handler
// 驱动卸载
_Use_decl_annotations_ static void DriverpDriverUnload(
    PDRIVER_OBJECT driver_object) {
  UNREFERENCED_PARAMETER(driver_object);
  PAGED_CODE();

  HYPERPLATFORM_COMMON_DBG_BREAK();

  VmTermination();
  HotplugCallbackTermination();
  PowerCallbackTermination();
  UtilTermination();
  PerfTermination();
  GlobalObjectTermination();
  LogTermination();
}

// Test if the system is one of supported OS versions
// 判断是否是支持的系统
_Use_decl_annotations_ bool DriverpIsSuppoetedOS() {
  PAGED_CODE();

  RTL_OSVERSIONINFOW os_version = {};
  auto status = RtlGetVersion(&os_version);
  if (!NT_SUCCESS(status)) {
    return false;
  }
  if (os_version.dwMajorVersion != 6 && os_version.dwMajorVersion != 10) {
    return false;
  }
  // 4-gigabyte tuning (4GT) should not be enabled
  if (!IsX64() &&
      reinterpret_cast<ULONG_PTR>(MmSystemRangeStart) != 0x80000000) {
    return false;
  }
  return true;
}

}  // extern "C"
