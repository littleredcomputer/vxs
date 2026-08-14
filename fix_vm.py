with open('src/vm.cpp', 'r') as f:
    text = f.read()

text = text.replace('insn->SubrValue() = ', 'insn->init_subr(')
text = text.replace('cdr(subr)->SubrValue();', 'cdr(subr)->SubrValue());') # wait, this is specific to line 297
text = text.replace('insn->get_cdr_word() = ', 'insn->set_cdr_word(')
text = text.replace('c->unsafe_vector_value() = cv;', 'c->init_vector(cv);')
text = text.replace('c->get_car_word() |= ', 'c->set_car_word(c->get_car_word() | ')
text = text.replace('c->get_cdr_word() = ', 'c->set_cdr_word(')
text = text.replace('c->SymbolValue() = ', 'c->init_symbol(')

with open('src/vm.cpp', 'w') as f:
    f.write(text)
