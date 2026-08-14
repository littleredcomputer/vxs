import re

with open('src/interp.cpp', 'r') as f:
    text = f.read()

text = text.replace('r_exp->cd.cv', 'r_exp->unsafe_vector_value()')
text = text.replace('res->cd.m', 'res->unsafe_magic_box()')
text = text.replace('res->cd.vp', 'res->unsafe_magic_vp()')
text = text.replace('d->cd.m', 'd->unsafe_magic_box()')
text = text.replace('d->cd.vp', 'd->unsafe_magic_vp()')
text = text.replace('c->cd.cv = cv;', 'c->init_vector(cv);')
text = text.replace('cont->cd.cv', 'cont->unsafe_vector_value()')

with open('src/interp.cpp', 'w') as f:
    f.write(text)

