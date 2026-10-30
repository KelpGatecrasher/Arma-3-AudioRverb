"""Print a top-level class (by name) from a derapified config.cpp."""
import sys
path, name = sys.argv[1], sys.argv[2]
lines = open(path, encoding="latin-1").read().splitlines()
for i, l in enumerate(lines):
    if l.strip() == f"class {name}" or l.strip().startswith(f"class {name}:"):
        depth, j = 0, i
        while True:
            print(lines[j])
            depth += lines[j].count("{") - lines[j].count("}")
            j += 1
            if depth == 0 and j > i + 1:
                break
        print("-----")
