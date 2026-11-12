# Boot trace: SDK calls during the first 30 seconds

Generated from `build/GF7E01/sdk_trace.txt` (`sfa` writes it on every run). With threads, interrupts, the
decrementer, ARAM, the DSP mailbox and the disc drive in place, the recompiled game runs `__start` → `OSInit`
→ `main` → its frame loop. It starts the CRI ADX threads, loads its first files (effects, the first texture
and model packs, and `ns_logos.nut`) and renders each frame (`GXCallDisplayList`).

In 30 seconds it makes **615,307 SDK calls** to **368 distinct functions**. `main` makes its first
SDK call at call #487.

| Library | Distinct functions used |
|---|---:|
| GX | 116 |
| OS | 70 |
| other | 35 |
| DVD | 28 |
| PPC | 19 |
| AX | 16 |
| AI | 15 |
| VI | 12 |
| DSP | 11 |
| AR/ARQ | 11 |
| EXI | 10 |
| OS cache | 8 |
| SI | 7 |
| CARD | 4 |
| MTX | 3 |
| PAD | 3 |

## Distinct SDK functions, in first-call order

| # | Function | Library | Calls | First caller |
|---:|---|---|---:|---|
| 1 | `__OSPSInit` | OS | 1 | `__init_hardware` |
| 2 | `PPCMfhid2` | PPC | 4 | `__OSPSInit` |
| 3 | `PPCMthid2` | PPC | 4 | `__OSPSInit` |
| 4 | `ICFlashInvalidate` | OS cache | 1 | `__OSPSInit` |
| 5 | `__OSFPRInit` | OS | 1 | `__init_hardware` |
| 6 | `__OSCacheInit` | OS | 2 | `__init_hardware` |
| 7 | `PPCMfhid0` | PPC | 6 | `__OSCacheInit` |
| 8 | `ICEnable` | OS cache | 1 | `__OSCacheInit` |
| 9 | `DBPrintf` | other | 8 | `__OSCacheInit` |
| 11 | `DCEnable` | OS cache | 1 | `__OSCacheInit` |
| 13 | `PPCMfl2cr` | PPC | 9 | `__OSCacheInit` |
| 14 | `PPCMfmsr` | PPC | 1 | `__OSCacheInit` |
| 15 | `PPCMtmsr` | PPC | 2 | `__OSCacheInit` |
| 17 | `PPCMtl2cr` | PPC | 5 | `__OSCacheInit` |
| 18 | `L2GlobalInvalidate` | other | 1 | `__OSCacheInit` |
| 31 | `OSSetErrorHandler` | OS | 2 | `__OSCacheInit` |
| 32 | `OSDisableInterrupts` | OS | 255,136 | `OSSetErrorHandler` |
| 33 | `OSRestoreInterrupts` | OS | 253,380 | `OSSetErrorHandler` |
| 35 | `DBInit` | other | 1 | `__start` |
| 36 | `OSInit` | OS | 1 | `__start` |
| 37 | `__OSGetSystemTime` | OS | 908 | `OSInit` |
| 39 | `OSGetTime` | OS | 3,506 | `__OSGetSystemTime` |
| 42 | `__OSGetExecParams` | OS | 1 | `OSInit` |
| 43 | `PPCMtmmcr0` | PPC | 1 | `OSInit` |
| 44 | `PPCMtmmcr1` | PPC | 1 | `OSInit` |
| 45 | `PPCMtpmc1` | PPC | 1 | `OSInit` |
| 46 | `PPCMtpmc2` | PPC | 1 | `OSInit` |
| 47 | `PPCMtpmc3` | PPC | 1 | `OSInit` |
| 48 | `PPCMtpmc4` | PPC | 1 | `OSInit` |
| 49 | `PPCDisableSpeculation` | PPC | 1 | `OSInit` |
| 51 | `PPCMthid0` | PPC | 2 | `PPCDisableSpeculation` |
| 52 | `PPCSetFpNonIEEEMode` | PPC | 1 | `OSInit` |
| 53 | `OSSetArenaLo` | OS | 4 | `OSInit` |
| 55 | `OSSetArenaHi` | OS | 1 | `OSInit` |
| 56 | `OSExceptionInit` | OS | 1 | `OSInit` |
| 58 | `DCFlushRangeNoSync` | OS cache | 2,526 | `OSExceptionInit` |
| 59 | `ICInvalidateRange` | OS cache | 17 | `OSExceptionInit` |
| 60 | `__DBIsExceptionMarked` | other | 15 | `OSExceptionInit` |
| 105 | `__OSSetExceptionHandler` | OS | 18 | `OSExceptionInit` |
| 121 | `__OSInitSystemCall` | OS | 1 | `OSInit` |
| 124 | `OSInitAlarm` | OS | 3 | `OSInit` |
| 125 | `__OSGetExceptionHandler` | OS | 3 | `OSInitAlarm` |
| 127 | `OSRegisterResetFunction` | OS | 4 | `OSInitAlarm` |
| 128 | `__OSModuleInit` | OS | 1 | `OSInit` |
| 129 | `__OSInterruptInit` | OS | 1 | `OSInit` |
| 130 | `__OSMaskInterrupts` | OS | 102 | `__OSInterruptInit` |
| 132 | `SetInterruptMask` | other | 17 | `__OSMaskInterrupts` |
| 141 | `__OSSetInterruptHandler` | OS | 22 | `OSInit` |
| 142 | `__OSContextInit` | OS | 1 | `OSInit` |
| 153 | `EXIInit` | EXI | 1 | `OSInit` |
| 165 | `EXIGetID` | EXI | 3 | `EXIInit` |
| 167 | `EXILock` | EXI | 50 | `EXIGetID` |
| 169 | `SetExiInterruptMask` | other | 100 | `EXILock` |
| 174 | `EXISelect` | EXI | 50 | `EXIGetID` |
| 177 | `EXIImm` | EXI | 51 | `EXIGetID` |
| 180 | `EXISync` | EXI | 100 | `EXIGetID` |
| 182 | `__OSGetDIConfig` | OS | 100 | `EXISync` |
| 191 | `EXIDeselect` | EXI | 50 | `EXIGetID` |
| 202 | `__EXIProbe` | other | 1,758 | `EXIGetID` |
| 209 | `OSRegisterVersion` | OS | 14 | `EXIInit` |
| 210 | `OSReport` | OS | 20 | `OSRegisterVersion` |
| 211 | `SIInit` | SI | 1 | `OSInit` |
| 214 | `SISetSamplingRate` | SI | 883 | `SIInit` |
| 216 | `VIGetTvFormat` | VI | 885 | `SISetSamplingRate` |
| 219 | `SISetXY` | SI | 883 | `SISetSamplingRate` |
| 224 | `__OSUnmaskInterrupts` | OS | 13 | `SIInit` |
| 228 | `SIGetType` | SI | 5 | `SIInit` |
| 238 | `SITransfer` | SI | 4 | `SIGetType` |
| 244 | `__SITransfer` | other | 4 | `SITransfer` |
| 312 | `__OSInitSram` | OS | 1 | `OSInit` |
| 313 | `DCInvalidateRange` | OS cache | 60 | `__OSInitSram` |
| 331 | `EXIDma` | EXI | 49 | `__OSInitSram` |
| 341 | `EXIUnlock` | EXI | 49 | `__OSInitSram` |
| 348 | `OSGetGbsMode` | OS | 1 | `__OSInitSram` |
| 350 | `UnlockSram` | other | 7 | `OSGetGbsMode` |
| 352 | `OSSetGbsMode` | OS | 1 | `__OSInitSram` |
| 356 | `__OSThreadInit` | OS | 1 | `OSInit` |
| 357 | `OSInitThreadQueue` | OS | 41 | `__OSThreadInit` |
| 358 | `OSClearContext` | OS | 7,034 | `__OSThreadInit` |
| 359 | `OSSetCurrentContext` | OS | 7,037 | `__OSThreadInit` |
| 360 | `DefaultSwitchThreadCallback` | other | 3,509 | `__OSThreadInit` |
| 361 | `OSClearStack` | OS | 1 | `__OSThreadInit` |
| 362 | `OSGetStackPointer` | OS | 1 | `OSClearStack` |
| 404 | `OSGetArenaHi` | OS | 3 | `OSInit` |
| 405 | `OSGetArenaLo` | OS | 4 | `OSInit` |
| 409 | `OSGetResetCode` | OS | 3 | `OSInit` |
| 413 | `OSEnableInterrupts` | OS | 1,755 | `OSInit` |
| 414 | `DVDInit` | DVD | 2 | `OSInit` |
| 417 | `__DVDFSInit` | DVD | 1 | `DVDInit` |
| 418 | `__DVDClearWaitingQueue` | DVD | 1 | `DVDInit` |
| 419 | `__DVDInitWA` | DVD | 1 | `DVDInit` |
| 420 | `__DVDLowSetWAType` | DVD | 1 | `__DVDInitWA` |
| 432 | `DVDInquiryAsync` | DVD | 1 | `OSInit` |
| 435 | `__DVDPushWaitingQueue` | DVD | 5 | `DVDInquiryAsync` |
| 438 | `stateReady` | DVD | 10 | `DVDInquiryAsync` |
| 439 | `__DVDCheckWaitingQueue` | DVD | 10 | `stateReady` |
| 442 | `__DVDPopWaitingQueue` | DVD | 5 | `stateReady` |
| 447 | `stateBusy` | DVD | 5 | `stateReady` |
| 448 | `DVDLowInquiry` | DVD | 1 | `stateBusy` |
| 449 | `OSCreateAlarm` | OS | 7 | `DVDLowInquiry` |
| 450 | `OSSetAlarm` | OS | 5 | `DVDLowInquiry` |
| 456 | `InsertAlarm` | other | 5 | `OSSetAlarm` |
| 461 | `PPCMtdec` | PPC | 5 | `InsertAlarm` |
| 464 | `__init_user` | other | 1 | `__start` |
| 465 | `__init_cpp` | other | 1 | `__init_user` |
| 466 | `OSDisableScheduler` | OS | 1,764 | `__init_user` |
| 469 | `__DVDInterruptHandler` | DVD | 5 | `__init_user` |
| 470 | `OSCancelAlarm` | OS | 5 | `__DVDInterruptHandler` |
| 475 | `cbForStateBusy` | DVD | 5 | `__DVDInterruptHandler` |
| 476 | `InquiryCallback` | other | 1 | `cbForStateBusy` |
| 483 | `OSEnableScheduler` | OS | 1,764 | `__init_user` |
| 486 | `__OSReschedule` | OS | 1,764 | `__init_user` |
| 491 | `OSInitAlloc` | OS | 1 | `fn_80177628` |
| 493 | `OSCreateHeap` | OS | 1 | `fn_80177628` |
| 494 | `OSSetCurrentHeap` | OS | 1 | `fn_80177628` |
| 497 | `VIInit` | VI | 1 | `fn_80173C4C` |
| 500 | `__VIInit` | other | 1 | `VIInit` |
| 501 | `getTiming` | other | 4 | `__VIInit` |
| 502 | `__OSLockSram` | OS | 1 | `VIInit` |
| 504 | `__OSUnlockSram` | OS | 1 | `VIInit` |
| 514 | `OSAllocFromHeap` | OS | 37 | `fn_80173C4C` |
| 515 | `GXInit` | GX | 2 | `fn_80173C4C` |
| 518 | `GXSetMisc` | GX | 2 | `GXInit` |
| 519 | `__GXFifoInit` | GX | 2 | `GXInit` |
| 525 | `OSGetCurrentThread` | OS | 2 | `__GXFifoInit` |
| 526 | `GXInitFifoBase` | GX | 2 | `GXInit` |
| 527 | `GXInitFifoLimits` | GX | 2 | `GXInitFifoBase` |
| 528 | `GXInitFifoPtrs` | GX | 2 | `GXInitFifoBase` |
| 531 | `GXSetCPUFifo` | GX | 2 | `GXInit` |
| 533 | `__GXWriteFifoIntEnable` | GX | 6 | `GXSetCPUFifo` |
| 534 | `PPCSync` | PPC | 4,278 | `GXSetCPUFifo` |
| 536 | `GXSetGPFifo` | GX | 2 | `GXInit` |
| 538 | `__GXFifoReadDisable` | GX | 2 | `GXSetGPFifo` |
| 542 | `__GXFifoLink` | GX | 3 | `GXSetGPFifo` |
| 543 | `__GXWriteFifoIntReset` | GX | 2 | `GXSetGPFifo` |
| 544 | `__GXFifoReadEnable` | GX | 2 | `GXSetGPFifo` |
| 547 | `__GXPEInit` | GX | 2 | `GXInit` |
| 560 | `PPCMtwpar` | PPC | 2 | `GXInit` |
| 562 | `__GXFlushTextureState` | GX | 20 | `GXInit` |
| 564 | `__GXInitRevisionBits` | GX | 2 | `GXInit` |
| 565 | `GXInitTexCacheRegion` | GX | 48 | `GXInit` |
| 589 | `GXInitTlutRegion` | GX | 40 | `GXInit` |
| 609 | `__GXSetIndirectMask` | GX | 2 | `GXInit` |
| 610 | `__GXSetTmemConfig` | GX | 2 | `GXInit` |
| 611 | `__GXInitGX` | GX | 2 | `GXInit` |
| 615 | `GXSetCopyClear` | GX | 3 | `__GXInitGX` |
| 616 | `GXSetTexCoordGen2` | GX | 16 | `__GXInitGX` |
| 617 | `__GXSetMatrixIndex` | GX | 18 | `GXSetTexCoordGen2` |
| 632 | `GXSetNumTexGens` | GX | 2 | `__GXInitGX` |
| 633 | `GXClearVtxDesc` | GX | 2 | `__GXInitGX` |
| 634 | `GXInvalidateVtxCache` | GX | 2 | `__GXInitGX` |
| 635 | `GXSetArray` | GX | 32 | `__GXInitGX` |
| 651 | `GXSetVtxAttrFmtv` | GX | 16 | `__GXInitGX` |
| 659 | `GXSetLineWidth` | GX | 2 | `__GXInitGX` |
| 660 | `GXSetPointSize` | GX | 2 | `__GXInitGX` |
| 661 | `GXEnableTexOffsets` | GX | 16 | `__GXInitGX` |
| 669 | `GXLoadPosMtxImm` | GX | 2 | `__GXInitGX` |
| 670 | `GXLoadNrmMtxImm` | GX | 2 | `__GXInitGX` |
| 671 | `GXSetCurrentMtx` | GX | 2 | `__GXInitGX` |
| 673 | `GXLoadTexMtxImm` | GX | 4 | `__GXInitGX` |
| 675 | `GXSetViewport` | GX | 2 | `__GXInitGX` |
| 676 | `__GXSetViewport` | GX | 882 | `GXSetViewport` |
| 677 | `GXSetProjectionv` | GX | 2 | `__GXInitGX` |
| 678 | `GXSetCoPlanar` | GX | 2 | `__GXInitGX` |
| 679 | `GXSetCullMode` | GX | 2 | `__GXInitGX` |
| 680 | `GXSetClipMode` | GX | 2 | `__GXInitGX` |
| 681 | `GXSetScissor` | GX | 5 | `__GXInitGX` |
| 682 | `GXSetScissorBoxOffset` | GX | 4 | `__GXInitGX` |
| 683 | `GXSetNumChans` | GX | 2 | `__GXInitGX` |
| 684 | `GXSetChanCtrl` | GX | 4 | `__GXInitGX` |
| 685 | `GXSetChanAmbColor` | GX | 4 | `__GXInitGX` |
| 686 | `GXSetChanMatColor` | GX | 4 | `__GXInitGX` |
| 690 | `GXInvalidateTexAll` | GX | 5 | `__GXInitGX` |
| 693 | `GXSetTexRegionCallback` | GX | 2 | `__GXInitGX` |
| 694 | `GXSetTlutRegionCallback` | GX | 2 | `__GXInitGX` |
| 695 | `GXInitTexObj` | GX | 2 | `__GXInitGX` |
| 696 | `GXLoadTexObj` | GX | 16 | `__GXInitGX` |
| 697 | `__GXDefaultTexRegionCallback` | GX | 16 | `GXLoadTexObj` |
| 698 | `GXGetTexObjFmt` | GX | 16 | `__GXDefaultTexRegionCallback` |
| 699 | `GXGetTexObjMipMap` | GX | 16 | `__GXDefaultTexRegionCallback` |
| 700 | `GXLoadTexObjPreLoaded` | GX | 16 | `GXLoadTexObj` |
| 736 | `GXSetTevOrder` | GX | 32 | `__GXInitGX` |
| 752 | `GXSetNumTevStages` | GX | 3 | `__GXInitGX` |
| 753 | `GXSetTevOp` | GX | 2 | `__GXInitGX` |
| 754 | `GXSetAlphaCompare` | GX | 2 | `__GXInitGX` |
| 755 | `GXSetZTexture` | GX | 2 | `__GXInitGX` |
| 756 | `GXSetTevKColorSel` | GX | 32 | `__GXInitGX` |
| 757 | `GXSetTevKAlphaSel` | GX | 32 | `__GXInitGX` |
| 758 | `GXSetTevSwapMode` | GX | 32 | `__GXInitGX` |
| 804 | `GXSetTevSwapModeTable` | GX | 8 | `__GXInitGX` |
| 808 | `GXSetTevDirect` | GX | 32 | `__GXInitGX` |
| 809 | `GXSetTevIndirect` | GX | 32 | `GXSetTevDirect` |
| 840 | `GXSetNumIndStages` | GX | 2 | `__GXInitGX` |
| 841 | `GXSetIndTexCoordScale` | GX | 8 | `__GXInitGX` |
| 845 | `GXSetFog` | GX | 2 | `__GXInitGX` |
| 846 | `GXSetFogRangeAdj` | GX | 2 | `__GXInitGX` |
| 847 | `GXSetBlendMode` | GX | 3 | `__GXInitGX` |
| 848 | `GXSetColorUpdate` | GX | 880 | `__GXInitGX` |
| 849 | `GXSetAlphaUpdate` | GX | 881 | `__GXInitGX` |
| 850 | `GXSetZMode` | GX | 880 | `__GXInitGX` |
| 851 | `GXSetZCompLoc` | GX | 2 | `__GXInitGX` |
| 852 | `GXSetDither` | GX | 3 | `__GXInitGX` |
| 853 | `GXSetDstAlpha` | GX | 2 | `__GXInitGX` |
| 854 | `GXSetPixelFmt` | GX | 4 | `__GXInitGX` |
| 855 | `GXSetFieldMask` | GX | 2 | `__GXInitGX` |
| 856 | `GXSetFieldMode` | GX | 3 | `__GXInitGX` |
| 859 | `GXSetDispCopySrc` | GX | 4 | `__GXInitGX` |
| 860 | `GXSetDispCopyDst` | GX | 4 | `__GXInitGX` |
| 861 | `GXSetDispCopyYScale` | GX | 3 | `__GXInitGX` |
| 862 | `GXSetCopyClamp` | GX | 2 | `__GXInitGX` |
| 863 | `GXSetCopyFilter` | GX | 4 | `__GXInitGX` |
| 864 | `GXSetDispCopyGamma` | GX | 3 | `__GXInitGX` |
| 865 | `GXSetDispCopyFrame2Field` | GX | 2 | `__GXInitGX` |
| 866 | `GXClearBoundingBox` | GX | 2 | `__GXInitGX` |
| 867 | `GXPokeColorUpdate` | GX | 2 | `__GXInitGX` |
| 868 | `GXPokeAlphaUpdate` | GX | 2 | `__GXInitGX` |
| 869 | `GXPokeDither` | GX | 2 | `__GXInitGX` |
| 870 | `GXPokeBlendMode` | GX | 2 | `__GXInitGX` |
| 871 | `GXPokeAlphaMode` | GX | 2 | `__GXInitGX` |
| 872 | `GXPokeAlphaRead` | GX | 3 | `__GXInitGX` |
| 873 | `GXPokeDstAlpha` | GX | 2 | `__GXInitGX` |
| 874 | `GXPokeZMode` | GX | 2 | `__GXInitGX` |
| 875 | `GXSetGPMetric` | GX | 2 | `__GXInitGX` |
| 876 | `GXClearGPMetric` | GX | 2 | `__GXInitGX` |
| 877 | `GXAdjustForOverscan` | GX | 2 | `fn_80173C4C` |
| 887 | `VIConfigure` | VI | 2 | `fn_80174A28` |
| 890 | `setVerticalRegs` | VI | 6 | `VIConfigure` |
| 895 | `GXSetViewportJitter` | GX | 880 | `fn_801772FC` |
| 898 | `C_MTXOrtho` | MTX | 1,764 | `fn_80174E28` |
| 905 | `GXCopyDisp` | GX | 880 | `fn_80173C4C` |
| 909 | `VISetNextFrameBuffer` | VI | 880 | `fn_80173C4C` |
| 911 | `setFbbRegs` | VI | 881 | `VISetNextFrameBuffer` |
| 913 | `VIFlush` | VI | 884 | `fn_80173C4C` |
| 916 | `VIWaitForRetrace` | VI | 1,754 | `fn_80173C4C` |
| 918 | `OSSleepThread` | OS | 1,754 | `VIWaitForRetrace` |
| 920 | `SelectThread` | other | 5,261 | `OSSleepThread` |
| 921 | `OSGetCurrentContext` | OS | 3,508 | `SelectThread` |
| 928 | `__VIRetraceHandler` | other | 1,756 | `SelectThread` |
| 931 | `fn_8013ABC8` | other | 882 | `__VIRetraceHandler` |
| 941 | `OSWakeupThread` | OS | 1,765 | `__VIRetraceHandler` |
| 959 | `VISetBlack` | VI | 4 | `fn_80173C4C` |
| 963 | `PSMTXIdentity` | MTX | 8 | `fn_80196888` |
| 971 | `DCFlushRange` | OS cache | 4,508 | `fn_8017FA2C` |
| 989 | `GXGetCPUFifo` | GX | 1 | `fn_801740E4` |
| 990 | `fn_80156AE8` | other | 1 | `fn_801740E4` |
| 991 | `fn_80156AF0` | other | 1 | `fn_801740E4` |
| 992 | `GXFlush` | GX | 880 | `fn_801740E4` |
| 993 | `__GXSetDirtyState` | GX | 2 | `GXFlush` |
| 994 | `__GXSetSUTexRegs` | GX | 2 | `__GXSetDirtyState` |
| 995 | `__SetSURegs` | other | 2 | `__GXSetSUTexRegs` |
| 996 | `__GXUpdateBPMask` | GX | 2 | `__GXSetDirtyState` |
| 997 | `__GXSetGenMode` | GX | 2 | `__GXSetDirtyState` |
| 998 | `__GXSetVCD` | GX | 2 | `__GXSetDirtyState` |
| 999 | `__GXSetVAT` | GX | 2 | `__GXSetDirtyState` |
| 1000 | `__GXCalculateVLim` | GX | 2 | `__GXSetDirtyState` |
| 1361 | `OSFreeToHeap` | OS | 2 | `fn_80174C50` |
| 1362 | `DLInsert` | other | 2 | `OSFreeToHeap` |
| 1381 | `GXGetYScaleFactor` | GX | 1 | `fn_801740E4` |
| 1452 | `DVDSetAutoFatalMessaging` | DVD | 1 | `main` |
| 1457 | `OSCheckHeap` | OS | 859 | `fn_801776B8` |
| 1458 | `PADInit` | PAD | 1 | `fn_80181100` |
| 1472 | `PADReset` | PAD | 879 | `PADInit` |
| 1474 | `SIDisablePolling` | SI | 879 | `PADReset` |
| 1475 | `SIGetTypeAsync` | SI | 1 | `PADReset` |
| 1486 | `PADControlMotor` | PAD | 4 | `fn_80016BD8` |
| 1498 | `CARDInit` | CARD | 1 | `fn_80196F34` |
| 1499 | `OSGetFontEncode` | OS | 1 | `CARDInit` |
| 1502 | `DSPInit` | DSP | 1 | `CARDInit` |
| 1503 | `__DSP_debug_printf` | DSP | 7 | `DSPInit` |
| 1519 | `__CARDSetDiskID` | CARD | 1 | `CARDInit` |
| 1544 | `CARDProbeEx` | CARD | 1,756 | `fn_80197AB4` |
| 1546 | `EXIProbeEx` | EXI | 1,756 | `CARDProbeEx` |
| 1551 | `CARDGetResultCode` | CARD | 1,756 | `fn_80197918` |
| 1571 | `GXSetDrawSyncCallback` | GX | 878 | `fn_801751BC` |
| 1574 | `GXCallDisplayList` | GX | 8,524 | `fn_801751BC` |
| 1579 | `GXSetDrawSync` | GX | 878 | `fn_801751BC` |
| 1636 | `VIGetRetraceCount` | VI | 1,754 | `fn_801759C0` |
| 1638 | `OSGetTick` | OS | 1,754 | `fn_8017597C` |
| 1768 | `OSInitStopwatch` | OS | 2 | `main` |
| 1769 | `OSStartStopwatch` | OS | 2 | `main` |
| 1775 | `PSVECNormalize` | MTX | 3 | `fn_8015E904` |
| 1779 | `AIInit` | AI | 1 | `fn_8018A25C` |
| 1782 | `__AI_set_stream_sample_rate` | AI | 1 | `AIInit` |
| 1783 | `AIGetStreamSampleRate` | AI | 2 | `__AI_set_stream_sample_rate` |
| 1784 | `AIGetStreamVolLeft` | AI | 2 | `__AI_set_stream_sample_rate` |
| 1785 | `AIGetStreamVolRight` | AI | 2 | `__AI_set_stream_sample_rate` |
| 1786 | `AISetStreamVolRight` | AI | 4 | `__AI_set_stream_sample_rate` |
| 1787 | `AISetStreamVolLeft` | AI | 4 | `__AI_set_stream_sample_rate` |
| 1789 | `__AI_SRC_INIT` | AI | 2 | `__AI_set_stream_sample_rate` |
| 1807 | `AISetStreamPlayState` | AI | 1 | `__AI_set_stream_sample_rate` |
| 1808 | `AIGetStreamPlayState` | AI | 1 | `AISetStreamPlayState` |
| 1811 | `AISetDSPSampleRate` | AI | 1 | `AIInit` |
| 1812 | `AIGetDSPSampleRate` | AI | 1 | `AISetDSPSampleRate` |
| 1839 | `AXInit` | AX | 1 | `fn_8018A25C` |
| 1840 | `AXInitEx` | AX | 1 | `AXInit` |
| 1843 | `fn_80146030` | other | 1 | `AXInitEx` |
| 1844 | `__AXVPBInit` | AX | 1 | `AXInitEx` |
| 1845 | `__AXPushFreeStack` | AX | 64 | `__AXVPBInit` |
| 1910 | `fn_80147B34` | other | 1 | `AXInitEx` |
| 1911 | `__AXAuxInit` | AX | 1 | `AXInitEx` |
| 1912 | `__AXClInit` | AX | 1 | `AXInitEx` |
| 1913 | `__AXOutInit` | AX | 1 | `AXInitEx` |
| 1916 | `__AXOutInitDSP` | AX | 1 | `__AXOutInit` |
| 1918 | `DSPCheckInit` | DSP | 1 | `__AXOutInitDSP` |
| 1919 | `DSPAddTask` | DSP | 1 | `__AXOutInitDSP` |
| 1921 | `__DSP_insert_task` | DSP | 1 | `DSPAddTask` |
| 1923 | `__DSP_boot_task` | DSP | 1 | `DSPAddTask` |
| 1924 | `DSPCheckMailFromDSP` | DSP | 2 | `__DSP_boot_task` |
| 1925 | `DSPReadMailFromDSP` | DSP | 2 | `__DSP_boot_task` |
| 1926 | `DSPSendMailToDSP` | DSP | 10 | `__DSP_boot_task` |
| 1927 | `DSPCheckMailToDSP` | DSP | 10 | `__DSP_boot_task` |
| 1955 | `__DSPHandler` | DSP | 1 | `__AXOutInitDSP` |
| 1960 | `__AXDSPInitCallback` | AX | 1 | `__DSPHandler` |
| 1967 | `AIRegisterDMACallback` | AI | 1 | `__AXOutInit` |
| 1970 | `__AXNextFrame` | AX | 1 | `__AXOutInit` |
| 1971 | `fn_80147730` | other | 1 | `__AXNextFrame` |
| 1972 | `__AXGetPBs` | AX | 1 | `__AXNextFrame` |
| 1973 | `fn_8014649C` | other | 1 | `__AXNextFrame` |
| 1974 | `__AXGetAuxBInput` | AX | 1 | `__AXNextFrame` |
| 1976 | `AIInitDMA` | AI | 1 | `__AXOutInit` |
| 1979 | `AIStartDMA` | AI | 1 | `__AXOutInit` |
| 1980 | `OSGetSoundMode` | OS | 2 | `fn_80191A6C` |
| 1984 | `MIXInit` | other | 1 | `fn_8018A25C` |
| 1985 | `__MIXSetPan` | other | 64 | `MIXInit` |
| 2049 | `fn_801476EC` | other | 1 | `fn_8018A25C` |
| 2052 | `AXRegisterAuxACallback` | AX | 1 | `fn_80190FE0` |
| 2053 | `AXFXChorusInit` | AX | 1 | `fn_80190FE0` |
| 2055 | `fn_8014BB38` | other | 1 | `AXFXChorusInit` |
| 2058 | `AXFXChorusSettings` | AX | 1 | `AXFXChorusInit` |
| 2061 | `AXRegisterAuxBCallback` | AX | 1 | `fn_80190FE0` |
| 2062 | `ARInit` | AR/ARQ | 1 | `fn_8018D714` |
| 2071 | `__ARChecksize` | AR/ARQ | 1 | `ARInit` |
| 2091 | `ARAlloc` | AR/ARQ | 1 | `fn_8018D714` |
| 2094 | `ARQInit` | AR/ARQ | 1 | `fn_8018D714` |
| 2100 | `__ARHandler` | AR/ARQ | 2 | `vprintf` |
| 2124 | `ARRegisterDMACallback` | AR/ARQ | 1 | `ARQInit` |
| 2128 | `ARGetBaseAddress` | AR/ARQ | 2 | `fn_8018D714` |
| 2129 | `ARQPostRequest` | AR/ARQ | 1 | `fn_8018D714` |
| 2131 | `ARStartDMA` | AR/ARQ | 1 | `ARQPostRequest` |
| 2142 | `__ARQInterruptServiceRoutine` | AR/ARQ | 1 | `__ARHandler` |
| 2143 | `__ARQServiceQueueLo` | AR/ARQ | 1 | `__ARQInterruptServiceRoutine` |
| 2150 | `OSYieldThread` | OS | 1 | `fn_8018D714` |
| 2164 | `fn_80146F74` | other | 1 | `fn_8018A740` |
| 2165 | `OSSetSoundMode` | OS | 1 | `fn_8018A688` |
| 2171 | `VISetPostRetraceCallback` | VI | 1 | `main` |
| 2179 | `OSInitFont` | OS | 1 | `fn_800A7208` |
| 2180 | `ReadFont` | other | 1 | `OSInitFont` |
| 2181 | `__OSReadROM` | OS | 48 | `ReadFont` |
| 3909 | `DCStoreRange` | OS cache | 1 | `fn_800A9104` |
| 4509 | `DVDGetDriveStatus` | DVD | 1,752 | `fn_8018E990` |
| 4616 | `OSGetResetButtonState` | OS | 875 | `main` |
| 10606 | `VIGetDTVStatus` | VI | 1 | `main` |
| 10609 | `OSSetProgressiveMode` | OS | 1 | `main` |
| 11294 | `DVDConvertPathToEntrynum` | DVD | 18 | `fn_80194654` |
| 11296 | `DVDFastOpen` | DVD | 9 | `fn_80194654` |
| 11297 | `DVDClose` | DVD | 9 | `fn_80194930` |
| 11298 | `DVDCancel` | DVD | 9 | `DVDClose` |
| 11299 | `DVDCancelAsync` | DVD | 9 | `DVDCancel` |
| 11301 | `cbForCancelSync` | DVD | 9 | `DVDCancelAsync` |
| 11975 | `DVDGetCommandBlockStatus` | DVD | 5,594 | `fn_801953FC` |
| 13503 | `DVDReadAsyncPrio` | DVD | 4 | `fn_80194EF4` |
| 13504 | `DVDReadAbsAsyncPrio` | DVD | 4 | `DVDReadAsyncPrio` |
| 13520 | `DVDLowRead` | DVD | 4 | `stateBusy` |
| 13521 | `Read` | other | 4 | `DVDLowRead` |
| 13547 | `DVDGetTransferredSize` | DVD | 1 | `fn_8001A24C` |
| 13565 | `cbForReadAsync` | DVD | 4 | `cbForStateBusy` |
| 14449 | `OSCheckStopwatch` | OS | 2,574 | `fn_8001A24C` |
| 234321 | `DecrementerExceptionCallback` | other | 1 | `SelectThread` |
