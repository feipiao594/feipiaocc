token:
- keyword [$nondigit$]
- identifier [$nondigit$,\u,\U,@special_character@]
- constant [',L,u,U,.,$digit$,\u,\U,@special_character@]
- string-literal [",u,U,L]
- punctuator [$punctuator_first$]

preprocessing-token:
- header-name [<,"]
- identifier [$nondigit$,\u,\U,@special_character@]
- pp-number [.,$digit$]
- character-constant [',L,u,U]
- string-literal [",u,U,L]
- punctuator [$punctuator_first$]
- other(non-white-space) [@default_character@]

keyword(*): [$nondigit$]

identifier: [$nondigit$,\u,\U,@special_character@]
- identifier-nondigit [$nondigit$,\u,\U]
- identifier identifier-nondigit [$nondigit$,\u,\U]
- identifier digit [$nondigit$,\u,\U]
- other(implementation-defined characters) [@special_character@]

identifier-nondigit: [$nondigit$,\u,\U]
- nondigit [$nondigit$]
- universal-character-name [\u,\U]

nondigit(*): [a-z,A-z,_]

digit(*): [0-9]

universal-character-name: [\u,\U]
- \u hex-quad [\u]
- \U hex-quad hex-quad [\U]

hex-quad: [0-9,a-f,A-f]
- hexadecimal-digit hexadecimal-digit hexadecimal-digit hexadecimal-digit 

constant: [',L,u,U,.,$digit$,\u,\U,@special_character@]
- integer-constant [0-9]
- floating-constant [.,$digit$]
- enumeration-constant [$nondigit$,\u,\U,@special_character@]
- character-constant [',L,u,U]

integer-constant: [0-9]
- decimal-constant integer-suffix(opt) [1-9]
- octal-constant integer-suffix(opt) [0]
- hexadecimal-constant integer-suffix(opt) [0]

decimal-constant: [1-9]
- nonzero-digit [1-9]
- decimal-constant digit [1-9]

octal-constant: [0]
- 0 [0]
- octal-constant octal-digit [0]

hexadecimal-constant: [0]
- hexadecimal-prefix hexadecimal-digit [0]
- hexadecimal-constant hexadecimal-digit [0]

hexadecimal-prefix(*): [0]

nonzero-digit(*): [1-9]

octal-digit(*): [0-8]

hexadecimal-digit(*): [0-9,a-f,A-f]

integer-suffix: [u,U,l,L]
- unsigned-suffix long-suffix(opt) [u,U]
- unsigned-suffix long-long-suffix [u,U]
- long-suffix unsigned-suffix(opt) [l,L]
- long-long-suffix unsigned-suffix(opt) [l,L]

unsigned-suffix(*): [u,U]

long-suffix(*): [l,L]

long-long-suffix(*): [l,L]

floating-constant: [.,$digit$]
- decimal-floating-constant [.,$digit$]
- hexadecimal-floating-constant [0]

decimal-floating-constant: [.,$digit$]
- fractional-constant exponent-part(opt) floating-suffix(opt) [.,$digit$]
- digit-sequence exponent-part floating-suffix(opt) [$digit$]

hexadecimal-floating-constant: [0]
- hexadecimal-prefix hexadecimal-fractional-constant binary-exponent-part floating-suffix(opt) [0]
- hexadecimal-prefix hexadecimal-digit-sequence binary-exponent-part floating-suffix(opt) [0]

fractional-constant: [.,$digit$]
- digit-sequence(opt) . digit-sequence [.,$digit$]
- digit-sequence . [$digit$]

exponent-part: [e,E]
- e sign(opt) digit-sequence [e]
- E sign(opt) digit-sequence [E]

sign(*): [+,-]

digit-sequence: [$digit$]
- digit [$digit$]
- digit-sequence digit [$digit$]

hexadecimal-fractional-constant: [.,$hexadecimal-digit$]
- hexadecimal-digit-sequence(opt) . hexadecimal-digit-sequence [.,$hexadecimal-digit$]
- hexadecimal-digit-sequence . [$hexadecimal-digit$]

binary-exponent-part: [p,P]
- p sign digit-sequence [p]
- P sign digit-sequence [P]

hexadecimal-digit-sequence: [$hexadecimal-digit$]
- hexadecimal-digit [$hexadecimal-digit$]
- hexadecimal-digit-sequence hexadecimal-digit [$hexadecimal-digit$]

floating-suffix(*): [f,l,F,L]

enumeration-constant: [$nondigit$,\u,\U,@special_character@]
- identifier [$nondigit$,\u,\U,@special_character@]

character-constant: [',L,u,U]
- ' c-char-sequence ' [']
- L' c-char-sequence ' [L]
- u' c-char-sequence ' [u]
- U' c-char-sequence ' [U]

c-char-sequence: [$c-char$]
- c-char [$c-char$]
- c-char-sequence c-char [$c-char$]

c-char(*): [$c-char$]
- any member of the source character set except the single-quote ', backslash \, or new-line character
- escape-sequence

escape-sequence(*): {without explantation in $c-char$}
- simple-escape-sequence {without explantation in $c-char$}
- octal-escape-sequence {without explantation in $c-char$}
- hexadecimal-escape-sequence {without explantation in $c-char$}
- universal-character-name {without explantation in $c-char$}

simple-escape-sequence(*): {without explantation in $c-char$}
\' \" \? \\ \a \b \f \n \r \t \v

octal-escape-sequence: {without explantation in $c-char$}
- \ octal-digit
- \ octal-digit octal-digit
- \ octal-digit octal-digit octal-digit

hexadecimal-escape-sequence: {without explantation in $c-char$}
- \x hexadecimal-digit
- hexadecimal-escape-sequence hexadecimal-digit

string-literal: [",u,U,L]
- encoding-prefix(opt) " s-char-sequence(opt)" [",u,U,L]

encoding-prefix(*): [u,U,L]

s-char-sequence: [$s-char$]
- s-char [$s-char$]
- s-char-sequence s-char [$s-char$]

s-char: [$s-char$]
- any member of the source character set except the double-quote ", backslash \, or new-line character
- escape-sequence

punctuator(*): [$punctuator_first$]

header-name: [<,"]
- < h-char-sequence > [<]
- " q-char-sequence " ["]

h-char-sequence: [$h-char$]
- h-char [$h-char$]
- h-char-sequence h-char [$h-char$]

- h-char: [$h-char$]
- any member of the source character set except the new-line character and >

q-char-sequence: [$q-char$]
- q-char [$q-char$]
- q-char-sequence q-char [$q-char$]

q-char: [$q-char$]
- any member of the source character set except the new-line character and "

pp-number: [.,$digit$]
- digit [$digit$]
- . digit [.]
- pp-number digit [.,$digit$]
- pp-number identifier-nondigit [.,$digit$]
- pp-number e sign [.,$digit$]
- pp-number E sign [.,$digit$]
- pp-number p sign [.,$digit$]
- pp-number P sign [.,$digit$]
- pp-number . [.,$digit$]