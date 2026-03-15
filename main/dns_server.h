/*
 * Minimal DNS Server for Captive Portal
 *
 * Answers every DNS query with 192.168.4.1 so that phones/laptops
 * automatically open the WiFi setup page when connecting to the
 * SoftAP.  Runs as a FreeRTOS task on UDP port 53.
 */
#pragma once

/// Start the captive-portal DNS server (call once, after SoftAP is up).
void start_dns_server();
