import re

def fix_file(filename):
    with open(filename, 'r') as f:
        text = f.read()

    # ca.i -> get_car_word()
    text = re.sub(r'([A-Za-z0-9_\(\)\-\>]+)->ca\.i', r'\1->get_car_word()', text)
    # cd.i -> get_cdr_word()
    text = re.sub(r'([A-Za-z0-9_\(\)\-\>]+)->cd\.i', r'\1->get_cdr_word()', text)
    
    # cd.cv -> unsafe_vector_value()
    text = re.sub(r'([A-Za-z0-9_\(\)\-\>]+)->cd\.cv', r'\1->unsafe_vector_value()', text)
    
    # cd.y -> SymbolValue() (when read)
    # wait, insn->cd.y->key -> insn->SymbolValue()->key
    text = re.sub(r'([A-Za-z0-9_\(\)\-\>]+)->cd\.y', r'\1->SymbolValue()', text)

    # cd.f -> SubrValue()
    text = re.sub(r'([A-Za-z0-9_\(\)\-\>]+)->cd\.f', r'\1->SubrValue()', text)

    # cd.m -> unsafe_magic_box()
    text = re.sub(r'([A-Za-z0-9_\(\)\-\>]+)->cd\.m', r'\1->unsafe_magic_box()', text)

    # cd.vp -> unsafe_magic_vp()
    text = re.sub(r'([A-Za-z0-9_\(\)\-\>]+)->cd\.vp', r'\1->unsafe_magic_vp()', text)

    # cd.j -> ContValue()
    text = re.sub(r'([A-Za-z0-9_\(\)\-\>]+)->cd\.j', r'\1->ContValue()', text)
    
    with open(filename, 'w') as f:
        f.write(text)

fix_file('src/vm.cpp')
fix_file('src/interp.cpp')
