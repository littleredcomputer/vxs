import re
with open('src/cell.cpp', 'r') as f:
    text = f.read()

# is_marked
text = text.replace('P->get_car_word() & Cell::MARK', 'P->is_marked()')
text = text.replace('Q->get_car_word() & Cell::MARK', 'Q->is_marked()')

# set_marked
text = text.replace('P->set_car_word(P->get_car_word() | Cell::MARK);', 'P->set_marked(true);')
text = text.replace('p->set_car_word(p->get_car_word() & ~Cell::MARK);', 'p->set_marked(false);')

# is_traversing_cdr
text = text.replace('P->get_cdr_word() & Cell::MARK', 'P->is_traversing_cdr()')
text = text.replace('Q->get_cdr_word() & Cell::MARK', 'Q->is_traversing_cdr()')

# set_traversing_cdr
text = text.replace('P->set_cdr_word(P->get_cdr_word() | Cell::MARK);', 'P->set_traversing_cdr(true);')
text = text.replace('Q->set_cdr_word(Q->get_cdr_word() & ~Cell::MARK);', 'Q->set_traversing_cdr(false);')

with open('src/cell.cpp', 'w') as f:
    f.write(text)
