/*****************************************************************************
 * File name: radar_device_config.h
 *
 * Description: This file contains types and function prototypes
 *
 * ===========================================================================
 * Copyright (C) 2023 Infineon Technologies AG. All rights reserved.
 * ===========================================================================
 *
 * ===========================================================================
 * Infineon Technologies AG (INFINEON) is supplying this file for use
 * exclusively with Infineon's sensor products. This file can be freely
 * distributed within development tools and software supporting such
 * products.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS".  NO WARRANTIES, WHETHER EXPRESS, IMPLIED
 * OR STATUTORY, INCLUDING, BUT NOT LIMITED TO, IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE APPLY TO THIS SOFTWARE.
 * INFINEON SHALL NOT, IN ANY CIRCUMSTANCES, BE LIABLE FOR DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES, FOR ANY REASON
 * WHATSOEVER.
 * ===========================================================================
 */
#ifndef SOURCE_RADAR_DEVICE_CONFIG_H_
#define SOURCE_RADAR_DEVICE_CONFIG_H_

/**
 * @def DEVICE_BGT60XXXXX
 * @brief Identifier for the device BGT60XXXXX.
 *
 * The macros below define the identifier values for each of the supported radar devices.
 * It is used to differentiate this specific device from others in the codebase.
 */
#define DEVICE_BGT60TR13C 0
#define DEVICE_BGT60UTR11 2

/**
 * @def CONNECTED_RADAR_DEVICE
 * @brief Identifier for the radar device connected to the microcontroller board.
 *
 * Use only the SUPPORTED devices as defined above @DEVICE_BGT60XXXXX.
 * This macro is used in radar_high_framerate_config.h and radar_low_framerate_config.h
 * to select the correct device register lists to be programmed.
 */
#define CONNECTED_RADAR_DEVICE DEVICE_BGT60TR13C

#endif /* SOURCE_RADAR_DEVICE_CONFIG_H_ */
