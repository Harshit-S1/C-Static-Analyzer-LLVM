import streamlit as st
import subprocess
import os

st.set_page_config(layout="wide")
st.title("C Code Static Analyzer & Optimizer")

col1, col2 = st.columns(2)

with col1:
    st.subheader("Input C Code")
    default_code = """
    #include <stdio.h>
    int main() {
    int x = 3 + 5;
    int y;
    while(x > 0) {
        y = 10;
        x = x - 1;
    }
    scanf("%d", &y);
    printf("%d", y);
    return 0;
}
int dead_func() {
    return 42;
}"""
    code_input = st.text_area("Paste C code here:", value=default_code, height=450)
    
    if st.button("Run Analysis", type="primary"):
        with open("temp.c", "w") as f:
            f.write(code_input)
            
        try:
            result = subprocess.run(["./analyzer", "temp.c", "--"], capture_output=True, text=True)
            st.session_state['output'] = result.stdout
            st.session_state['errors'] = result.stderr
        except FileNotFoundError:
            st.error("Error: 'analyzer.exe' not found. Please compile the C++ code first and place the executable in this folder.")

with col2:
    st.subheader("Analysis Output")
    if 'output' in st.session_state:
        if st.session_state['errors']:
            st.error(st.session_state['errors'])
        else:
            st.text_area("Results", value=st.session_state['output'], height=450)