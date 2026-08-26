import os
import re
from PIL import Image

import sys

def main():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    default_img_dir = os.path.join(repo_root, "firmware", "01_flash_image_slideshow", "images")
    default_out_dir = os.path.join(repo_root, "firmware", "01_flash_image_slideshow")

    images_dir = sys.argv[1] if len(sys.argv) > 1 else default_img_dir
    output_dir = sys.argv[2] if len(sys.argv) > 2 else default_out_dir
    os.makedirs(output_dir, exist_ok=True)
    
    if not os.path.exists(images_dir):
        print(f"Error: {images_dir} directory not found.")
        return
        
    # Helper to sort naturally if names are numbers, e.g. "1.jpg", "10.jpg"
    def natural_sort_key(s):
        return [int(text) if text.isdigit() else text.lower() for text in re.split(r'(\d+)', s)]
        
    valid_exts = ('.png', '.jpg', '.jpeg', '.bmp')
    img_files = [f for f in os.listdir(images_dir) if f.lower().endswith(valid_exts)]
    img_files.sort(key=natural_sort_key)
    
    if not img_files:
        print(f"No valid images found in {images_dir}")
        return
        
    print(f"Found {len(img_files)} images: {img_files}")
    
    header_path = os.path.join(output_dir, "image_data.h")
    
    # Adjust JPEG quality here. 45 fits within the standard 3MB partition.
    # Set to a higher value (e.g., 80) if using a larger partition scheme in Arduino IDE.
    jpeg_quality = 45
    
    with open(header_path, "w") as f_out:
        f_out.write("#ifndef IMAGE_DATA_H\n")
        f_out.write("#define IMAGE_DATA_H\n\n")
        f_out.write("#include <Arduino.h>\n\n")
        
        array_names = []
        
        for idx, fname in enumerate(img_files, start=1):
            img_path = os.path.join(images_dir, fname)
            img = Image.open(img_path)
            
            # Target resolution for horizontal mode is 1920x480
            if img.size != (1920, 480):
                print(f"Warning: Image {fname} size is {img.size}. Resizing to 1920x480...")
                img = img.resize((1920, 480), Image.Resampling.LANCZOS)
                
            # Convert to RGB if needed
            if img.mode != 'RGB':
                img = img.convert('RGB')
                
            # Save as JPEG in a temporary/optimized way and get its bytes
            jpeg_temp = os.path.join(output_dir, f"temp_{idx}.jpg")
            img.save(jpeg_temp, "JPEG", quality=jpeg_quality)
            
            with open(jpeg_temp, "rb") as f_img:
                jpeg_data = f_img.read()
                
            # Clean up temp file
            os.remove(jpeg_temp)
            
            array_name = f"image_jpg_{idx}"
            array_names.append((array_name, len(jpeg_data), fname))
            
            print(f"Converted {fname} -> {array_name} (size: {len(jpeg_data)} bytes)")
            
            f_out.write(f"// Image {idx}: {fname} ({img.size[0]}x{img.size[1]}), size: {len(jpeg_data)} bytes\n")
            f_out.write(f"const uint8_t {array_name}[] PROGMEM = {{\n")
            
            # Write bytes in chunks of 12 for neat formatting
            for chunk in range(0, len(jpeg_data), 12):
                line = ", ".join(f"0x{b:02X}" for b in jpeg_data[chunk:chunk+12])
                f_out.write(f"    {line},\n")
                
            f_out.write("};\n\n")
            
        # Write pointers array
        f_out.write("// Pointers to image arrays in PROGMEM\n")
        f_out.write("const uint8_t* const image_jpg_ptrs[] PROGMEM = {\n")
        for array_name, _, _ in array_names:
            f_out.write(f"    {array_name},\n")
        f_out.write("};\n\n")
        
        # Write lengths array
        f_out.write("// Length of each JPEG image array\n")
        f_out.write("const uint32_t image_jpg_lens[] PROGMEM = {\n")
        for _, length, _ in array_names:
            f_out.write(f"    {length},\n")
        f_out.write("};\n\n")
        
        # Write total count
        f_out.write(f"const int NUM_IMAGES = {len(img_files)};\n\n")
        
        f_out.write("#endif // IMAGE_DATA_H\n")
        
    print(f"Generated C header array in {header_path} with {len(img_files)} images.")

if __name__ == "__main__":
    main()
