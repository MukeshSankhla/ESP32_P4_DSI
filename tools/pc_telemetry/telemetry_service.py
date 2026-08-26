import os
import sys
import time
from datetime import datetime
import psutil

# Add the directory containing the DLL and its dependencies to sys.path so pythonnet can resolve them
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
DLL_DIR = os.path.join(BASE_DIR, "LibreHardwareMonitor")
sys.path.append(DLL_DIR)

# Initialize pythonnet CLR
import clr
try:
    clr.AddReference("LibreHardwareMonitorLib")
except Exception as e:
    print(f"Error loading LibreHardwareMonitorLib.dll: {e}")
    print("Please make sure the DLL and its dependencies are present in the 'LibreHardwareMonitor' folder.")
    sys.exit(1)

from LibreHardwareMonitor.Hardware import Computer

class TelemetryService:
    def __init__(self):
        self.computer = None
        self.initialize_hardware()

    def initialize_hardware(self):
        """Initializes the LibreHardwareMonitor Computer object with CPU, GPU, Memory, Motherboard, and Controller enabled."""
        try:
            self.computer = Computer()
            self.computer.IsCpuEnabled = True
            self.computer.IsGpuEnabled = True
            self.computer.IsMemoryEnabled = True
            self.computer.IsMotherboardEnabled = True
            self.computer.IsControllerEnabled = True
            self.computer.Open()
        except Exception as e:
            print(f"Error during hardware initialization: {e}")
            self.computer = None

    def read_cpu_temp_fallback(self):
        """
        Fallback method to read CPU temperature via WMI/CIM when LibreHardwareMonitor driver fails.
        Queries the root/wmi MSAcpi_ThermalZoneTemperature class using PowerShell.
        """
        try:
            import subprocess
            cmd = [
                "powershell",
                "-NoProfile",
                "-Command",
                "Get-CimInstance -Namespace root/wmi -ClassName MSAcpi_ThermalZoneTemperature | Select-Object -ExpandProperty CurrentTemperature"
            ]
            # Prevent command window popup on Windows
            startupinfo = None
            if os.name == 'nt':
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW

            result = subprocess.run(cmd, capture_output=True, text=True, startupinfo=startupinfo, timeout=2)
            if result.returncode == 0:
                lines = result.stdout.strip().split('\n')
                temps = []
                for line in lines:
                    line = line.strip()
                    if line.isdigit():
                        val = float(line)
                        # WMI returns temperature in tenths of Kelvin (e.g., 3000 = 300.0 Kelvin)
                        temp_c = (val / 10.0) - 273.15
                        if 0 < temp_c < 115:
                            temps.append(temp_c)
                if temps:
                    return max(temps)
        except Exception:
            pass
        return None

    def read_cpu(self):
        """
        Reads CPU temperature and usage sensors.
        Returns:
            tuple: (cpu_temp, cpu_usage)
        """
        temp = None
        usage = None
        if not self.computer:
            return temp, usage

        try:
            for hardware in self.computer.Hardware:
                if str(hardware.HardwareType) == "Cpu":
                    hardware.Update()
                    for sensor in hardware.Sensors:
                        if str(sensor.SensorType) == "Temperature":
                            # Prioritize CPU Package or Core Max, fallback to Core Average or first temperature sensor
                            if "Package" in sensor.Name:
                                if sensor.Value is not None:
                                    temp = float(sensor.Value)
                            elif "Max" in sensor.Name and temp is None:
                                if sensor.Value is not None:
                                    temp = float(sensor.Value)
                            elif temp is None:
                                if sensor.Value is not None:
                                    temp = float(sensor.Value)
                                    
                        elif str(sensor.SensorType) == "Load":
                            if "Total" in sensor.Name:
                                if sensor.Value is not None:
                                    usage = float(sensor.Value)
        except Exception as e:
            # Silence exception to prevent crashes
            pass

        # Fallback to Motherboard / SuperIO temperature sensors if CPU hardware doesn't report it
        if temp is None:
            try:
                for hardware in self.computer.Hardware:
                    h_type = str(hardware.HardwareType)
                    if h_type in ["SuperIO", "Motherboard"]:
                        hardware.Update()
                        for sensor in hardware.Sensors:
                            if str(sensor.SensorType) == "Temperature":
                                # Look for "CPU" in sensor name (e.g. CPU, CPU Core, CPU PECI)
                                if "CPU" in sensor.Name or "cpu" in sensor.Name.lower():
                                    if sensor.Value is not None:
                                        temp = float(sensor.Value)
                                        break
            except Exception:
                pass

        # Fallback to WMI/CIM query if both LibreHardwareMonitor CPU and Motherboard sensors are not available
        if temp is None:
            temp = self.read_cpu_temp_fallback()

        return temp, usage

    def read_gpu(self):
        """
        Reads GPU temperature and usage sensors. Prioritizes discrete GPUs (NVIDIA/AMD) over integrated ones.
        Returns:
            tuple: (gpu_temp, gpu_usage)
        """
        temp = None
        usage = None
        if not self.computer:
            return temp, usage

        try:
            # First, look for a discrete GPU (NVIDIA/AMD)
            gpu_hardware = None
            for hardware in self.computer.Hardware:
                h_type = str(hardware.HardwareType)
                if h_type in ["GpuNvidia", "GpuAmd"]:
                    gpu_hardware = hardware
                    break
            
            # If no discrete GPU is found, fall back to integrated Intel/generic GPU
            if not gpu_hardware:
                for hardware in self.computer.Hardware:
                    h_type = str(hardware.HardwareType)
                    if h_type in ["GpuIntel", "Gpu"]:
                        gpu_hardware = hardware
                        break

            if gpu_hardware:
                gpu_hardware.Update()
                for sensor in gpu_hardware.Sensors:
                    if str(sensor.SensorType) == "Temperature":
                        # Prioritize GPU Core temperature
                        if "Core" in sensor.Name or temp is None:
                            if sensor.Value is not None:
                                temp = float(sensor.Value)
                    elif str(sensor.SensorType) == "Load":
                        # Prioritize GPU Core load
                        if "Core" in sensor.Name or usage is None:
                            if sensor.Value is not None:
                                usage = float(sensor.Value)
        except Exception as e:
            # Silence exception to prevent crashes
            pass
        return temp, usage

    def read_memory(self):
        """
        Reads memory usage load. Falls back to psutil if LibreHardwareMonitor is unavailable.
        Returns:
            float: memory_usage percentage
        """
        usage = None
        if self.computer:
            try:
                for hardware in self.computer.Hardware:
                    if str(hardware.HardwareType) == "Memory":
                        hardware.Update()
                        for sensor in hardware.Sensors:
                            if str(sensor.SensorType) == "Load" and "Memory" in sensor.Name:
                                if sensor.Value is not None:
                                    usage = float(sensor.Value)
                                    break
            except Exception:
                pass

        # Fallback to psutil if LibreHardwareMonitor sensor read failed
        if usage is None:
            try:
                usage = float(psutil.virtual_memory().percent)
            except Exception:
                pass

        return usage

    def get_telemetry_dict(self, cpu_temp, cpu_usage, gpu_temp, gpu_usage, memory_usage):
        """
        Builds the final telemetry dictionary.
        """
        now = datetime.now()
        # Format: DD-Mon-YYYY (e.g., 10-Jul-2026) and HH:MM:SS AM/PM
        date_str = now.strftime("%d-%b-%Y")
        time_str = now.strftime("%I:%M:%S %p")
        
        return {
            "cpu_temp": round(cpu_temp, 1) if cpu_temp is not None else None,
            "cpu_usage": int(round(cpu_usage)) if cpu_usage is not None else None,
            "gpu_temp": round(gpu_temp, 1) if gpu_temp is not None else None,
            "gpu_usage": int(round(gpu_usage)) if gpu_usage is not None else None,
            "memory_usage": int(round(memory_usage)) if memory_usage is not None else None,
            "date": date_str,
            "time": time_str
        }

    def print_dashboard(self, data):
        """Prints a clean formatted dashboard representation of the telemetry."""
        # Format string representation for each value
        cpu_temp_str = f"{data['cpu_temp']:.1f} °C" if data['cpu_temp'] is not None else "N/A"
        cpu_usage_str = f"{data['cpu_usage']} %" if data['cpu_usage'] is not None else "N/A"
        gpu_temp_str = f"{data['gpu_temp']:.1f} °C" if data['gpu_temp'] is not None else "N/A"
        gpu_usage_str = f"{data['gpu_usage']} %" if data['gpu_usage'] is not None else "N/A"
        mem_usage_str = f"{data['memory_usage']} %" if data['memory_usage'] is not None else "N/A"

        # Check if running as administrator to print helpful warning for CPU temp
        admin_warning = ""
        if data['cpu_temp'] is None:
            admin_warning = "\nNote: Run script as Administrator to enable CPU Temperature readings."

        # Clear console (Windows standard)
        os.system('cls')

        dashboard = f"""=====================================
      PC TELEMETRY DASHBOARD
=====================================

CPU Usage     : {cpu_usage_str}
CPU Temp      : {cpu_temp_str}

GPU Usage     : {gpu_usage_str}
GPU Temp      : {gpu_temp_str}

Memory Usage  : {mem_usage_str}

Date          : {data['date']}
Time          : {data['time']}

====================================={admin_warning}"""
        print(dashboard)

    def close(self):
        """Closes the computer hardware monitoring session."""
        if self.computer:
            try:
                self.computer.Close()
            except Exception:
                pass


def main():
    service = TelemetryService()
    try:
        # Pacing: print dashboard every 500ms (0.5s)
        # Telemetry is updated/read from sensors every 1s
        last_read_time = 0
        cpu_temp = cpu_usage = gpu_temp = gpu_usage = memory_usage = None
        
        while True:
            current_time = time.time()
            # Read sensors every 1.0 seconds
            if current_time - last_read_time >= 1.0:
                cpu_temp, cpu_usage = service.read_cpu()
                gpu_temp, gpu_usage = service.read_gpu()
                memory_usage = service.read_memory()
                last_read_time = current_time

            # Generate telemetry dictionary and display dashboard every 500ms
            telemetry_data = service.get_telemetry_dict(
                cpu_temp, cpu_usage, gpu_temp, gpu_usage, memory_usage
            )
            service.print_dashboard(telemetry_data)
            
            time.sleep(0.5)

    except KeyboardInterrupt:
        print("\nExiting telemetry dashboard...")
    finally:
        service.close()

if __name__ == "__main__":
    main()
