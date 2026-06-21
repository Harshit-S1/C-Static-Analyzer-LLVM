import streamlit as st
import subprocess
import tempfile
import os
import glob
from code_editor import code_editor

# this makes the app take up the whole screen
st.set_page_config(layout="wide", page_title="C to CFG Optimizer")

# Initialize Session State
# keeping track of our data so it doesn't vanish every time the user clicks something
if "analysis_done" not in st.session_state:
    st.session_state.analysis_done = False
    st.session_state.orig_graphs = {}
    st.session_state.opt_graphs = {}
    st.session_state.opt_code = ""
    st.session_state.res_log = ""

# Always Visible Sidebar
with st.sidebar:
    st.title("Control Panel")
    
    # A nice highlighted info box telling others how to run it
    st.info("**Press the PLAY BUTTON inside the code editor to run the passes!**")
    
    st.markdown("---")
    st.subheader("Graph Selectors")
    # empty spots to hold the dropdown menus later
    orig_selector_spot = st.empty()
    opt_selector_spot = st.empty()

st.title("C Code Static Analyzer & Optimizer")

col1, col2 = st.columns([1, 1]) 

with col1:
    st.header("C Code Input")
    default_code = """#include <stdio.h>

void f1() { printf("hello"); }

int main() {
    int arr[10];
    int x;
    
    scanf("%d", &x); 
    
    int y = x;
    int z = 10 + 20; 
    
    while(y < 20) {  
        arr[y] = 100; 
        y++;
    }
    
    int dead_var = z; 
    
    return 0;
}"""
    
    # Integrated Submit Button
    # built a custom button for the editor so it syncs text instantly when clicked
    custom_btns = [{
        "name": "Analyze & Optimize",
        "feather": "Play",
        "primary": True,
        "hasText": True,
        "showWithIcon": True,
        "commands": ["submit"],
        "style": {"bottom": "0.5rem", "right": "0.5rem"}
    }]
    
    editor_response = code_editor(
        default_code, 
        lang="c_cpp", 
        theme="default", 
        height=[20, 30], 
        shortcuts="vscode", 
        key="c_editor",
        buttons=custom_btns
    )
    
    # grabs the text if something is typed, otherwise default is used
    c_code = editor_response["text"] if editor_response["text"] else default_code

with col2:
    st.header("Analysis Output")

    # the heavy LLVM backend is only run if the play button is actually clicked
    if editor_response["type"] == "submit":
        if c_code.strip() == "":
            st.warning("Please enter some C code to analyze.")
        else:
            # everything is put in a temp folder, so the messy files are not left behind
            with tempfile.TemporaryDirectory() as temp_dir:
                analyzer_path = os.path.abspath("./analyzer")
                
                # Fixed-Point Iteration
                # the optimizer is kept running until the code entirely stops changing
                current_code = c_code
                iteration = 0
                max_iterations = 5 # Safety net to prevent infinite loops
                log_output = ""
                
                try:
                    with st.spinner("Running Dataflow & Optimization Passes..."):
                        while iteration < max_iterations:
                            iteration += 1
                            temp_c_path = os.path.join(temp_dir, "temp.c")
                            
                            # current state of the code is written to a file
                            with open(temp_c_path, "w") as f: 
                                f.write(current_code)
                                
                            # old dot files are nuked so they are not read accidently
                            for f in glob.glob(os.path.join(temp_dir, "*_cfg.dot")):
                                os.remove(f)

                            # C++ analyzer program is launched
                            res = subprocess.run([analyzer_path, "temp.c", "--"], cwd=temp_dir, capture_output=True, text=True)
                            log_output += f"**--- PASS {iteration} ---**\n{res.stdout}\n"
                            if res.stderr: log_output += f"Errors:\n{res.stderr}\n"
                            
                            # C syntax errors are caught early and cleanly shown
                            if res.returncode != 0 or "error:" in res.stderr:
                                st.error("Compilation Error! Please check your C code syntax.")
                                with st.expander("🚨 View Raw Error Log", expanded=True):
                                    st.text(log_output)
                                st.stop()
                                
                            # If this is the very first pass, saved as "Original" graphs
                            if iteration == 1:
                                st.session_state.orig_graphs.clear()
                                for dot_file in glob.glob(os.path.join(temp_dir, "*_cfg.dot")):
                                    with open(dot_file, "r") as f:
                                        st.session_state.orig_graphs[os.path.basename(dot_file)] = f.read()
                                        
                            # Checking what the C++ optimizer spit out
                            opt_c_path = os.path.join(temp_dir, "optimized.c")
                            if os.path.exists(opt_c_path):
                                with open(opt_c_path, "r") as f:
                                    new_code = f.read()
                            else:
                                new_code = current_code
                                
                            # See if the text changed, if not changed then convergence is reached
                            if new_code == current_code:
                                st.session_state.opt_graphs.clear()
                                for dot_file in glob.glob(os.path.join(temp_dir, "*_cfg.dot")):
                                    with open(dot_file, "r") as f:
                                        st.session_state.opt_graphs[os.path.basename(dot_file)] = f.read()
                                st.session_state.opt_code = new_code
                                break
                                
                            current_code = new_code
                            
                            # If the max iterations are done, just use whatever we have so far
                            if iteration == max_iterations:
                                st.session_state.opt_code = new_code
                                st.session_state.opt_graphs.clear()
                                for dot_file in glob.glob(os.path.join(temp_dir, "*_cfg.dot")):
                                    with open(dot_file, "r") as f:
                                        st.session_state.opt_graphs[os.path.basename(dot_file)] = f.read()

                    st.session_state.res_log = log_output
                    st.session_state.analysis_done = True

                except Exception as e:
                    # Catching Python-side crashes
                    st.error(f"An execution error occurred: {e}")
                    with st.expander("View Python Error", expanded=True):
                        st.text(str(e))

    # Render Block
    # UI components are drawn using the data saved in session_state
    if st.session_state.analysis_done:
        tab1, tab2, tab3 = st.tabs(["Original CFG", "Optimized CFG", "Optimized Code"])
        
        with tab1:
            if st.session_state.orig_graphs:
                # the _cfg.dot part is stripped out for the dropdown names for cleanliness
                func_names = [f.replace("_cfg.dot", "") for f in st.session_state.orig_graphs.keys()]
                selected_orig = orig_selector_spot.selectbox("View Original Function:", func_names, key="orig_select")
                st.graphviz_chart(st.session_state.orig_graphs[selected_orig + "_cfg.dot"], use_container_width=True)
                
        with tab2:
            if st.session_state.opt_graphs:
                func_names_opt = [f.replace("_cfg.dot", "") for f in st.session_state.opt_graphs.keys()]
                selected_opt = opt_selector_spot.selectbox("View Optimized Function:", func_names_opt, key="opt_select")
                st.graphviz_chart(st.session_state.opt_graphs[selected_opt + "_cfg.dot"], use_container_width=True)

        with tab3:
            st.code(st.session_state.opt_code, language="c")

        st.markdown("<br>", unsafe_allow_html=True)
        with st.expander("View Raw Console Analysis (Convergence Log)"):
            st.markdown(st.session_state.res_log)