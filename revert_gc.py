import re
with open('src/cell.cpp', 'r') as f:
    text = f.read()

# is_marked -> P->get_car_word() & Cell::MARK
text = text.replace('P->is_marked()', 'P->get_car_word() & Cell::MARK')
text = text.replace('Q->is_marked()', 'Q->get_car_word() & Cell::MARK')
text = text.replace('p->is_marked()', 'word & Cell::MARK')

# set_marked
text = text.replace('P->set_marked(true);', 'P->set_car_word(P->get_car_word() | Cell::MARK);')
text = text.replace('p->set_marked(false);', 'p->set_car_word(p->get_car_word() & ~Cell::MARK);')
text = text.replace('if (word & Cell::MARK)', 'if (p->get_car_word() & Cell::MARK)') # wait, the previous line made it word & Cell::MARK

# is_traversing_cdr -> get_cdr_word() & Cell::MARK
text = text.replace('P->is_traversing_cdr()', 'P->get_cdr_word() & Cell::MARK')
text = text.replace('Q->is_traversing_cdr()', 'Q->get_cdr_word() & Cell::MARK')

# set_traversing_cdr
text = text.replace('P->set_traversing_cdr(true);', 'P->set_cdr_word(P->get_cdr_word() | Cell::MARK);')
text = text.replace('Q->set_traversing_cdr(false);', 'Q->set_cdr_word(Q->get_cdr_word() & ~Cell::MARK);')

with open('src/cell.cpp', 'w') as f:
    f.write(text)

with open('src/vx-scheme.h', 'r') as f:
    text = f.read()

text = text.replace('''  bool is_marked() const { return m_gc_mark; }
  void set_marked(bool marked) { m_gc_mark = marked; }
  
  bool is_traversing_cdr() const { return m_gc_traverse_cdr; }
  void set_traversing_cdr(bool traversing) { m_gc_traverse_cdr = traversing; }\n\n''', '')

text = text.replace('''  bool m_gc_mark = false;
  bool m_gc_traverse_cdr = false;\n\n''', '')

with open('src/vx-scheme.h', 'w') as f:
    f.write(text)
