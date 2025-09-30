import re
import os
import sys
import json
import argparse
from openai import OpenAI

client = OpenAI(api_key=os.getenv('OPENAI_API_KEY'))

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
        You are a validation tool that compares a Zephyr SMF state-machine implementation (in C) against a PlantUML “source of truth.”
        Your job is to verify that states, transitions, and hierarchy match exactly, ignoring entry/running/exit callbacks.

        Input:
        • C implementation:
            - States defined via `struct smf_state`
            - Transitions invoked with `smf_set_state()`
        • PlantUML source:
            - States and sub-states
            - Initial transitions (`[*] -->`)
            - Transitions between states

        Validation steps:
        1. Extract all state identifiers from both C and PlantUML.
        2. Extract all transition pairs (source → target) from both.
        3. Extract the parent-child (hierarchy) relationships among states from both.
        4. Verify initial transitions (`[*] --> state`) exist in both.
        5. Ensure that every state, transition, and hierarchical relationship appears in both representations.
        6. Ensure that the UML input is syntactically valid.

        Output:
        A single JSON object with two fields:
            • `match` (boolean):
                - `true` if all states, transitions, and hierarchies align
                - `false` otherwise
            • `details` (string):
                - On success, a brief confirmation message.
                - On failure, list the missing or mismatched elements.

        Guidance:
        - Only consider states, transitions, and hierarchy. Ignore entry/running/exit functions.
        - Consider PlantUML deep history (`[H*]` inside a composite state, with transitions targeting that
          alias) as semantically equivalent to the C implementation resuming the last active leaf state
          within that composite state.
        - Consider a PlantUML choice pseudostate used for initial routing (e.g., `CHOOSE_INITIAL` with
          guards that point to alternative initial children) as semantically equivalent to a C setup where
          the initial child is decided conditionally (e.g., at build time via configuration or runtime).
          If either of the guarded targets matches a valid initial child in C, treat the initial alignment
          as satisfied.
        - If any ambiguity arises, default `match` to `false` and document the ambiguity in `details`.
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

    # Call OpenAI ChatCompletion
    response = client.chat.completions.create(
        model="gpt-5",
        messages=[
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt}
        ],
        # temperature=1,
        seed=313,
    )

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
