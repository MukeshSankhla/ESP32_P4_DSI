import sys
import time
import serial
import os
from telemetry_service import TelemetryService

def main():
    # Allow overriding COM port via CLI arguments (default is COM13)
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM13'
    baudrate = 115200

    print("Initializing telemetry service...")
    service = TelemetryService()

    ser = None
    last_read_time = 0
    cpu_temp = cpu_usage = gpu_temp = gpu_usage = memory_usage = None

    print(f"Starting telemetry stream on {port} @ {baudrate} baud...")
    
    try:
        while True:
            # Check serial connection and open/reopen if necessary
            if ser is None or not ser.is_open:
                try:
                    ser = serial.Serial(port, baudrate, timeout=1)
                    print(f"\n[+] Connected to ESP32 on {port}")
                except Exception as e:
                    sys.stdout.write(f"\r[-] Error opening serial port {port}: {e}. Retrying in 2s...")
                    sys.stdout.flush()
                    time.sleep(2)
                    continue

            current_time = time.time()
            # Read hardware sensors every 1.0 seconds
            if current_time - last_read_time >= 1.0:
                try:
                    cpu_temp, cpu_usage = service.read_cpu()
                    gpu_temp, gpu_usage = service.read_gpu()
                    memory_usage = service.read_memory()
                    last_read_time = current_time
                except Exception as e:
                    print(f"\n[-] Error reading hardware sensors: {e}")

            # Get structured telemetry dictionary
            telemetry_data = service.get_telemetry_dict(
                cpu_temp, cpu_usage, gpu_temp, gpu_usage, memory_usage
            )

            # Replace None with default values for robust serial transmission/parsing
            c_temp = telemetry_data["cpu_temp"] if telemetry_data["cpu_temp"] is not None else 0.0
            c_usage = telemetry_data["cpu_usage"] if telemetry_data["cpu_usage"] is not None else 0
            g_temp = telemetry_data["gpu_temp"] if telemetry_data["gpu_temp"] is not None else 0.0
            g_usage = telemetry_data["gpu_usage"] if telemetry_data["gpu_usage"] is not None else 0
            m_usage = telemetry_data["memory_usage"] if telemetry_data["memory_usage"] is not None else 0
            date_str = telemetry_data["date"]
            time_str = telemetry_data["time"]

            # Format the packet as CSV: $TEL:cpu_temp,cpu_usage,gpu_temp,gpu_usage,mem_usage,date,time\n
            packet = f"$TEL:{c_temp:.1f},{c_usage},{g_temp:.1f},{g_usage},{m_usage},{date_str},{time_str}\n"

            try:
                ser.write(packet.encode('ascii'))
                ser.flush()
            except Exception as e:
                print(f"\n[-] Serial write error: {e}. Reconnecting...")
                try:
                    ser.close()
                except:
                    pass
                ser = None
                time.sleep(1)
                continue

            # Print local dashboard to PC terminal
            service.print_dashboard(telemetry_data)
            print(f"Sent Packet: {packet.strip()}")

            time.sleep(1.0) # Streams once per second

    except KeyboardInterrupt:
        print("\n[!] Exiting telemetry stream...")
    finally:
        if ser and ser.is_open:
            ser.close()
        service.close()

if __name__ == "__main__":
    main()
