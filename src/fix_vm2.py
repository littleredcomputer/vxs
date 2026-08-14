import re
with open('src/vm.cpp', 'r') as f:
    text = f.read()

text = text.replace('insn->set_cdr_word(index;', 'insn->set_cdr_word(index);')
text = text.replace('insn->set_cdr_word(count;', 'insn->set_cdr_word(count);')
text = text.replace('c->set_cdr_word(opnd->IntValue();', 'c->set_cdr_word(opnd->IntValue());')
text = text.replace('c->set_car_word(c->get_car_word() | (opcode & 0xff) << 24;', 'c->set_car_word(c->get_car_word() | ((opcode & 0xff) << 24));')
text = text.replace('c->set_car_word(c->get_car_word() | count << 16;', 'c->set_car_word(c->get_car_word() | (count << 16));')
text = text.replace('c->init_symbol(opnd->SymbolValue();', 'c->init_symbol(opnd->SymbolValue());')
text = text.replace('c->init_symbol(y;', 'c->init_symbol(y);')
text = text.replace('c->set_cdr_word((u1 << 16) | u2;', 'c->set_cdr_word((u1 << 16) | u2);')
text = text.replace('(((_insn)->get_car_word() = ((_insn)->get_car_word() & 0xffffff) | (value << 24)))', '(_insn)->set_car_word(((_insn)->get_car_word() & 0xffffff) | (value << 24))')

with open('src/vm.cpp', 'w') as f:
    f.write(text)
