import sys
import cv2
import os

# Video pre-rotation mode (matches display mounting orientation)
# cv2.ROTATE_90_CLOCKWISE        -> 90 degrees Clockwise (default for flipped landscape screen)
# cv2.ROTATE_90_COUNTERCLOCKWISE -> 90 degrees Counter-Clockwise
# cv2.ROTATE_180                 -> 180 degrees
# None                           -> No rotation
ROTATION_MODE = cv2.ROTATE_90_CLOCKWISE

input_path = sys.argv[1] if len(sys.argv) > 1 else "animations/1.mp4"
output_path = sys.argv[2] if len(sys.argv) > 2 else "animations/as1.mjpeg"

if not os.path.exists(input_path):
    print(f"Error: {input_path} not found in the directory!")
    exit(1)

cap = cv2.VideoCapture(input_path)
if not cap.isOpened():
    print(f"Error: Could not open {input_path}!")
    exit(1)

# Get video information
total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
fps = cap.get(cv2.CAP_PROP_FPS)
width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))

target_fps = 24.0
duration = total_frames / fps if fps > 0 else 0
total_target_frames = int(round(duration * target_fps))

print(f"Loaded: {input_path}")
print(f"Original Resolution: {width}x{height} at {fps:.2f} FPS, Total Frames: {total_frames}")
print(f"Resampling to target {target_fps:.2f} FPS. Target Frames: {total_target_frames}")
print(f"Converting to {output_path} (Rotation: {ROTATION_MODE}, Scaled to 480x1920, JPEG Quality 80)...")

# Open output file in binary write mode
with open(output_path, "wb") as f:
    target_idx = 0
    input_idx = 0
    ret, frame = cap.read()
    
    while ret and target_idx < total_target_frames:
        needed_input_idx = int(target_idx * fps / target_fps)
        
        while input_idx < needed_input_idx:
            ret, frame = cap.read()
            input_idx += 1
            if not ret:
                break
                
        if not ret or frame is None:
            break
            
        # Rotate the frame based on config
        if ROTATION_MODE is not None:
            rotated = cv2.rotate(frame, ROTATION_MODE)
        else:
            rotated = frame
        
        # Resize to portrait 480x1920 (matching the display's resolution)
        resized = cv2.resize(rotated, (480, 1920))
        
        # Compress frame as JPEG (using 80% quality to balance image fidelity and file size)
        ret_val, jpeg_bytes = cv2.imencode(".jpg", resized, [cv2.IMWRITE_JPEG_QUALITY, 80])
        if ret_val:
            f.write(jpeg_bytes.tobytes())
            
        target_idx += 1
        if target_idx % 100 == 0 or target_idx == total_target_frames:
            percentage = (target_idx / total_target_frames) * 100
            print(f"Progress: {target_idx}/{total_target_frames} frames ({percentage:.1f}%)")

cap.release()
print(f"\nSuccess! Converted video saved as: {output_path}")
print(f"File size: {os.path.getsize(output_path) / (1024*1024):.2f} MB")
