import re

with open('src/cell.cpp', 'r') as f:
    text = f.read()

def replacer(match):
    prefix = match.group(1)
    field = match.group(2)
    op = match.group(3)
    val = match.group(4)

    getter = ""
    setter = ""
    
    if field == "ca.i":
        getter = "get_car_word()"
        setter = "set_car_word"
    elif field == "cd.i":
        getter = "get_cdr_word()"
        setter = "set_cdr_word"
    elif field == "ca.p":
        getter = "unsafe_car()"
        setter = "set_unsafe_car"
    elif field == "cd.p":
        getter = "unsafe_cdr()"
        setter = "set_unsafe_cdr"
    elif field == "cd.cv":
        getter = "unsafe_vector_value()"
        setter = "init_vector"
        
    if op == "=":
        return f"{prefix}->{setter}({val})"
    elif op == "|=":
        return f"{prefix}->{setter}({prefix}->{getter} | {val})"
    elif op == "&=":
        return f"{prefix}->{setter}({prefix}->{getter} & {val})"
    else:
        return match.group(0)

# Replace assignments
text = re.sub(r'([A-Za-z0-9_]+)->(ca\.i|cd\.i|ca\.p|cd\.p|cd\.cv)\s*([|=&]?=)\s*([^;]+)', replacer, text)

# Replace reads
text = re.sub(r'([A-Za-z0-9_]+)->ca\.i', r'\1->get_car_word()', text)
text = re.sub(r'([A-Za-z0-9_]+)->cd\.i', r'\1->get_cdr_word()', text)
text = re.sub(r'([A-Za-z0-9_]+)->ca\.p', r'\1->unsafe_car()', text)
text = re.sub(r'([A-Za-z0-9_]+)->cd\.p', r'\1->unsafe_cdr()', text)
text = re.sub(r'([A-Za-z0-9_]+)->cd\.cv', r'\1->unsafe_vector_value()', text)

with open('src/cell.cpp', 'w') as f:
    f.write(text)

