#!/usr/bin/env python3
import re
import sys

# Hacky BeebASM -> CA65 script used for sections of code conversion
# This needs work as it hardcodes the files.

def convert_line(line):
    # Remove trailing newline
    line = line.rstrip('\n')
    
    # If empty line, return empty
    if not line.strip():
        return ''
    
    # Check for whole line comment: starts with \; after optional whitespace
    m = re.match(r'(\s*)\\\\(.*)', line)
    if m:
        indent = m.group(1)
        comment = m.group(2)
        return indent + ';' + comment
    
    # Check for label: starts with . after optional whitespace, then label, then optional whitespace and then comment or end
    m = re.match(r'(\s*)(\.[a-zA-Z_][a-zA-Z0-9_]*)(\s*)(.*)', line)
    if m:
        indent = m.group(1)
        label = m.group(2)
        # The rest after the label: we assume it's only whitespace and/or comment
        rest = m.group(4)
        # If there is non-whitespace and non-comment, we don't handle (but assume there isn't)
        # Output the label line
        label_line = indent + label[1:] + ':'  # remove the dot and add colon
        # If there is a comment in the rest, we output a comment line after the label
        # Look for comment in rest
        comment_match = re.search(r'(;|\\\\)', rest)
        if comment_match:
            # Split rest into code_part and comment_part
            code_part = rest[:comment_match.start()]
            comment_part = rest[comment_match.start():]
            # Convert comment marker if needed
            if comment_part.startswith('\\\\'):
                comment_part = ';' + comment_part[2:]
            # If code_part is empty or only whitespace, then we output a comment line
            if code_part.strip() == '':
                comment_line = indent + ' ' * 8 + comment_part
                return label_line + '\n' + comment_line
            else:
                # There is code after the label on the same line -> we don't expect this, but if so, we treat as instruction line
                # We'll process the entire line as a normal line (but we already have the label) -> we'll just do the label line and then process the rest as a normal line?
                # For simplicity, we'll just output the label line and then the rest as a normal line (with 8 spaces indent? but note the rest already has indent)
                # We'll just return the label line and then the rest as a normal line (but note the rest includes the original indent after the label)
                # We'll do: label_line + '\n' + convert_line(indent + ' ' * 8 + rest)  # but note: we already converted the label, so we don't want to double convert
                # Instead, we'll just process the rest as a normal line with the same indent as the label line? 
                # We'll do: label_line + '\n' + convert_line(indent + ' ' * 8 + rest)
                # But note: the rest already has the whitespace_after (from group 3) and then the rest (group 4). We'll ignore group 3 and use 8 spaces.
                # We'll create a new line: indent + 8 spaces + rest
                new_line = indent + ' ' * 8 + rest
                return label_line + '\n' + convert_line(new_line)
        else:
            # No comment in rest
            if rest.strip() == '':
                return label_line
            else:
                # There is non-whitespace without comment -> treat as instruction line
                new_line = indent + ' ' * 8 + rest
                return label_line + '\n' + convert_line(new_line)
    
    # Check for constant definition: symbol = value
    m = re.match(r'(\s*)([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*(.*)', line)
    if m:
        indent = m.group(1)
        symbol = m.group(2)
        value = m.group(3)
        # Split value into value_part and comment_part
        comment_match = re.search(r'(;|\\\\)', value)
        if comment_match:
            value_part = value[:comment_match.start()]
            comment_part = value[comment_match.start():]
            if comment_part.startswith('\\\\'):
                comment_part = ';' + comment_part[2:]
        else:
            value_part = value
            comment_part = ''
        # Convert hex numbers in value_part: & to $
        value_part = value_part.replace('&', '$')
        # Build the .define line
        define_line = indent + '.define ' + symbol + ' ' + value_part
        if comment_part:
            define_line += ' ' + comment_part
        return define_line
    
    # Otherwise, treat as code line (instruction with possible comment)
    # Split into code and comment
    comment_match = re.search(r'(;|\\\\)', line)
    if comment_match:
        code_part = line[:comment_match.start()]
        comment_part = line[comment_match.start():]
        if comment_part.startswith('\\\\'):
            comment_part = ';' + comment_part[2:]
    else:
        code_part = line
        comment_part = ''
    
    # Process code_part: convert instruction to lowercase and change & to $ in operands
    # First, change & to $ in the entire code_part (for hex numbers)
    code_part = code_part.replace('&', '$')
    # Now, split into tokens to get the first token (the instruction)
    tokens = code_part.split()
    if tokens:
        # Convert the first token to lowercase
        tokens[0] = tokens[0].lower()
        code_part = ' '.join(tokens)
    # If there were no tokens, then code_part is empty (or only whitespace) -> leave as is
    
    # Build the result: indent with 8 spaces? 
    # But note: the original line might have indentation. We want to keep the original indentation for the code line? 
    # However, the example in filing_vectors.s shows the instruction indented by 8 spaces relative to the label? 
    # But in the BBC ASM, the instruction line already has indentation (a tab or spaces). We want to preserve that indentation? 
    # The requirement: "Follow the styling of '../../bbc/fn-rom/src/vectors/filing_vectors.s' which uses cc65's assembly format, with 8 spaces at the start"
    # This means that each line of code should start with 8 spaces? 
    # Looking at filing_vectors.s, the lines in the .segment "CODE" start with 8 spaces (or a tab? but the file shows 8 spaces).
    # We will output the code line with 8 spaces at the beginning, regardless of the original indentation.
    # However, note that the label line we output earlier does not have the 8 spaces? 
    # In filing_vectors.s, the label is at the beginning of the line (no indentation) and then the instruction is indented by 8 spaces.
    # So we will:
    #   For label lines: output with the original indentation (which we preserved) and then the label and colon.
    #   For code lines: output with 8 spaces at the beginning (and then the code_part and comment).
    # But wait, the original code line might have had indentation (like a tab) that we want to replace with 8 spaces.
    # We'll ignore the original indentation for code lines and use 8 spaces.
    # However, note that the original code line might have been indented to align with something else? 
    # Given the requirement, we'll output 8 spaces at the start of every code line.
    result = ' ' * 8 + code_part
    if comment_part:
        if code_part:
            result += ' ' + comment_part
        else:
            result += comment_part
    return result

def main():
    input_file = '/home/markf/dev/bbc/MMFS/mmfs100.asm'
    start_line = 4026
    end_line = 4277
    
    with open(input_file, 'r') as f:
        lines = f.readlines()
    
    # Adjust for 1-based line numbers in the file
    selected_lines = lines[start_line-1:end_line]
    
    output_lines = []
    for line in selected_lines:
        converted = convert_line(line)
        if converted == '':
            output_lines.append('')
        else:
            # The convert_line might return multiple lines? We split by newline and extend
            if '\n' in converted:
                output_lines.extend(converted.split('\n'))
            else:
                output_lines.append(converted)
    
    # Write the output to the target file
    target_file = '../../bbc/fn-rom/src/vectors/gbpb_functions.s'
    with open(target_file, 'w') as f:
        for line in output_lines:
            f.write(line + '\n')

if __name__ == '__main__':
    main()