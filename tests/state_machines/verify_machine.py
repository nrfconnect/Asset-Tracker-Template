import re
import os
import sys
import json
import argparse
import logging
from openai import AzureOpenAI

def setup_client():
    """Initialize OpenAI client with Azure OpenAI """
    try:
        # Try Azure OpenAI
        return AzureOpenAI(
            api_version="2024-02-15-preview",
            api_key=os.getenv("OPENAI_API_KEY"),
            azure_endpoint=os.getenv("AZURE_OPENAI_ENDPOINT")
        ), "azure"
    except Exception as e:
        logging.warning(f"Failed to initialize Azure OpenAI client: {e}")

        raise ValueError("No valid OpenAI credentials found. Please set either Azure OpenAI environment variables")

# Initialize the client
client, client_type = setup_client()
logging.info(f"Using {client_type.upper()} client")

def load_file(path):
    try:
        with open(path, 'r') as f:
            return f.read()
    except Exception as e:
        print(f"Error reading {path}: {e}")
        sys.exit(1)

def extract_json(text):
    # Strip markdown code fences if present
    # Match ``` ... ``` and capture inner content
    m = re.search(r"```(?:json)?\s*(\{[\s\S]*\})\s*```", text)
    if m:
        return m.group(1)
    return text

def normalize_match_value(val):
    if isinstance(val, bool):
        return val
    if isinstance(val, str):
        low = val.strip().lower().strip('"')
        return low == 'true'
    return False

def compare_state_machines(c_code, plantuml):
    # Construct prompt for LLM
    system_prompt = """
        You are a validation tool that compares a Zephyr SMF state-machine implementation (in C) against a PlantUML "source of truth."
        Your job is to verify that states, transitions, and hierarchy match semantically, ignoring entry/running/exit callbacks.

        Input:
        • C implementation:
            - States defined via `struct smf_state` with SMF_CREATE_STATE(entry, run, exit, parent, initial_child)
            - The 5th parameter (initial_child) defines the initial substate when entering a composite state
            - Transitions invoked with `smf_set_state()` in run functions
            - Parent state run functions can handle events and transition on behalf of child states
        • PlantUML source:
            - States and sub-states
            - Initial transitions (`[*] -->`) at various hierarchy levels
            - Transitions between states with event labels

        Validation steps:
        1. Extract all state identifiers from both C and PlantUML.
        2. Extract all transition pairs (source → target) from both.
        3. Extract the parent-child (hierarchy) relationships among states from both.
        4. Verify initial transitions exist in both - see equivalence rules below.
        5. Ensure that every state, transition, and hierarchical relationship appears in both representations.
        6. Ensure that the UML input is syntactically valid.

        Output:
        A single JSON object with two fields:
            • `match` (boolean):
                - `true` if all states, transitions, and hierarchies align semantically
                - `false` only if there are genuine mismatches (not just representation differences)
            • `details` (string):
                - On success, a brief confirmation message.
                - On failure, list only the GENUINE missing or mismatched elements.

        CRITICAL EQUIVALENCE RULES - Apply these before flagging mismatches:

        1. INITIAL STATE SEMANTICS:
           - When PlantUML shows `[*] --> PARENT` and PARENT contains `[*] --> CHILD`, this is equivalent to
             C code where STATE_PARENT has initial_child pointing to CHILD.
           - The combination means: "system starts in PARENT, which immediately enters CHILD"
           - This is functionally identical to C directly starting in CHILD with PARENT as its parent.

        2. CHOICE PSEUDOSTATES FOR INITIAL ROUTING:
           - PlantUML: `[*] --> CHOOSE_INITIAL` followed by conditional transitions to different children
             (e.g., `CHOOSE_INITIAL --> CHILD_A: config_a` and `CHOOSE_INITIAL --> CHILD_B: otherwise`)
           - C: `initial_child = &states[INITIAL_MODE]` where INITIAL_MODE is a preprocessor conditional
             (e.g., `#if CONFIG_X ... #define INITIAL_MODE STATE_A #else ... #define INITIAL_MODE STATE_B`)
           - These are semantically equivalent: both select the initial child conditionally.
           - If any of the choice targets matches the C initial child, consider it a match.

        3. DEEP HISTORY:
           - PlantUML `[H*]` inside a composite state, with transitions targeting that composite
           - C implementation: tracking last active leaf state and restoring it on re-entry
           - These are semantically equivalent.
           - It is allowed to re-enter into a parent state if the parent state has an initial transition to a child leaf state.

        IMPORTANT:
        - Focus on semantic equivalence, not syntactic exactness.
        - If the STATE MACHINES would behave identically at runtime, they match.
        - Only flag genuine mismatches (missing states, wrong targets, missing transitions).
        - If in doubt about equivalence, default `match` to `true` and note the equivalence in `details`.
        - Be permissive: the goal is to verify behavior, not syntax.
        """


    user_prompt = (
        "C Implementation:\n```c\n" + c_code + "```\n"
        "PlantUML Definition:\n```plantuml\n" + plantuml + "```\n\n"
        "Compare states, transitions, and hierarchy only (ignore entry/running/exit callbacks). "
        "Use the JSON schema:\n"
        "  {\n"
        "    \"match\": <true|false>,\n"
        "    \"details\": \"<description of any missing/mismatched elements or 'All aligned.'>\"\n"
        "  }\n"
        "If in doubt, set \"match\" to false and note the ambiguity in \"details.\" "
        "Return **only** the JSON object (no extra text)."
    )

    # Call OpenAI ChatCompletion with appropriate model name based on client type
    model_name = "gpt-5.2"
    try:
        response = client.chat.completions.create(
            model=model_name,
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt}
            ],
            # temperature=1,
            seed=313,
        )
    except Exception as e:
        logging.error(f"Error calling {client_type.upper()} API: {e}")
        raise

    # Respose is a string in json-like format
    # {"match": "true/false", "details": "...details..."}
    text = response.choices[0].message.content.strip()

    # Extract pure JSON
    json_text = extract_json(text)

    try:
        result = json.loads(json_text)
    except json.JSONDecodeError as e:
        print("JSON decoding error")
        raise e

    # Normalize match value in case it's a string
    result["match"] = normalize_match_value(result.get("match"))

    return result


def main():
    # Configure logging
    logging.basicConfig(level=logging.INFO, format='%(asctime)s - %(levelname)s - %(message)s')

    parser = argparse.ArgumentParser(
        description="Compare C state machine vs PlantUML definition using OpenAI API",
        allow_abbrev=False,
    )
    parser.add_argument("--c-file", required=True, help="Path to the C source file")
    parser.add_argument("--uml-file", required=True, help="Path to the PlantUML file")
    args = parser.parse_args()

    c_code = load_file(args.c_file)
    plantuml = load_file(args.uml_file)

    result = compare_state_machines(c_code, plantuml)

    if result.get("match") is True:
        print("✅ State machines match.")
        print(result.get("details", "No details provided."))
        sys.exit(0)
    else:
        print("❌ State machines do not match:")
        print(result.get("details", "No details provided."))
        sys.exit(1)

if __name__ == "__main__":
    main()
