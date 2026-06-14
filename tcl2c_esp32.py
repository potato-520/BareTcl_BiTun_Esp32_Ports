import sys

def convert(in_file, out_file, var_name):
    with open(in_file, 'rb') as f:
        data = f.read()
    
    with open(out_file, 'w') as f:
        f.write('/* Auto-generated */\n')
        f.write(f'const char {var_name}[] = {{\n')
        for i, b in enumerate(data):
            f.write(f"0x{b:02x}, ")
            if (i + 1) % 16 == 0:
                f.write('\n')
        f.write('0x00\n};\n')

if __name__ == '__main__':
    if len(sys.argv) != 4:
        print("Usage: python3 tcl2c_esp32.py <in.tcl> <out.c> <var_name>")
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2], sys.argv[3])
