import re
import os
import sys
import json
import time
import argparse
import logging
from openai import AzureOpenAI

logger = logging.getLogger("state_machine_verify")

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
        logger.warning(f"Failed to initialize Azure OpenAI client: {e}")

        raise ValueError("No valid OpenAI credentials found. Please set either Azure OpenAI environment variables")

# Initialize the client
client, client_type = setup_client()

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

def compare_state_machines(c_code, plantuml, max_retries=3, max_iterations=3, debug=False):
    """Compare state machines with multiple validation passes and retry logic."""
    logger.setLevel(logging.DEBUG if debug else logging.INFO)

    results = []
    logger.debug("\n" + "="*50)
    logger.debug("Starting state machine comparison")
    logger.debug("="*50)

    def make_api_call(prompt, analysis_type, attempt=1):
        """Make API call with exponential backoff retry."""
        try:
            response = client.chat.completions.create(
                model="gpt-5.1",
                messages=[
                    {"role": "system", "content": prompt},
                    {"role": "user", "content": (
                        "C Implementation:\n```c\n" + c_code + "```\n"
                        "PlantUML Definition:\n```plantuml\n" + plantuml + "```\n\n"
                        "Compare implementations and return JSON:\n"
                        "{\n"
                        "  \"match\": <true|false>,\n"
                        "  \"details\": \"<detailed analysis>\"\n"
                        "}\n"
                    )}
                ],
                temperature=0.0,  # Use 0 for maximum determinism
                seed=313,
            )

            text = response.choices[0].message.content.strip()
            json_text = extract_json(text)

            try:
                result = json.loads(json_text)
                result["match"] = normalize_match_value(result.get("match"))
                result["analysis_type"] = analysis_type
                return result
            except json.JSONDecodeError as e:
                logger.error(f"JSON decode error in {analysis_type}: {e}")
                if attempt < max_retries:
                    wait_time = 2 ** attempt  # Exponential backoff
                    logger.info(f"Retrying {analysis_type} in {wait_time} seconds...")
                    time.sleep(wait_time)
                    return make_api_call(prompt, analysis_type, attempt + 1)
                return None

        except Exception as e:
            logger.error(f"API error in {analysis_type}: {e}")
            if attempt < max_retries:
                wait_time = 2 ** attempt  # Exponential backoff
                logger.info(f"Retrying {analysis_type} in {wait_time} seconds...")
                time.sleep(wait_time)
                return make_api_call(prompt, analysis_type, attempt + 1)
            return None

    # First pass - Strict structural comparison
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
        4. Verify initial transitions exist in both.
        5. Ensure that every state, transition, and hierarchical relationship appears in both representations.

        Output:
        A single JSON object with two fields:
            • `match` (boolean):
                - `true` if all states, transitions, and hierarchies align semantically
                - `false` only if there are genuine mismatches (not just representation differences)
            • `details` (string):
                - On success, empty string
                - On failure, list only the GENUINE missing or mismatched elements.

        CRITICAL EQUIVALENCE RULES - Apply these before flagging mismatches:

        1. INITIAL STATE SEMANTICS:
           - When PlantUML shows `[*] --> PARENT` and PARENT contains `[*] --> CHILD`, this is equivalent to
             C code where STATE_PARENT has initial_child pointing to CHILD.
           - The combination means: "system starts in PARENT, which immediately enters CHILD"
           - This is functionally identical to C directly starting in CHILD with PARENT as its parent.

        2. DEEP HISTORY:
           - PlantUML `[H*]` inside a composite state, with transitions targeting that composite
           - C implementation: tracking last active leaf state and restoring it on re-entry
           - These are semantically equivalent.
           - It is allowed to re-enter into a parent state if the parent state has an initial transition to a child leaf state.

        IMPORTANT:
        - Focus on semantic equivalence, not syntactic exactness.
        - Only flag genuine mismatches (missing states, wrong targets, missing transitions).
        - If in doubt about equivalence, default `match` to `true` and note the equivalence in `details`.
        """


    prompts = [
        (system_prompt, "State Analysis")
    ]

    logger.debug("\n" + "-"*50)
    logger.debug("Starting analysis")
    logger.debug("-"*50)

    for system_prompt, analysis_type in prompts:
        logger.debug(f"\nExecuting {analysis_type}...")
        result = make_api_call(system_prompt, analysis_type)
        if result:
            results.append(result)


    # Convert results to a format suitable for review
    analyses_summary = "\n\n".join([
        "\n\n"
        f"Analysis Type: {r['analysis_type']}\n"
        f"Match: {r['match']}\n"
        f"Details: {r['details']}"
        for r in results
    ])

    logger.debug(analyses_summary)

    # Final verification to ensure state and transition analyses are consistent
    system_prompt_reasoning = """
        You are a validation tool that compares a Zephyr SMF state-machine implementation (in C) against a PlantUML "source of truth."
        Your job is to verify that the state and transition analyses are consistent with each other.

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
        • Previous Analysis Results:
            - State Analysis: Results from the state hierarchy verification
            - Previous review and iteration history

        Validation steps:
        1. Review previous analyses and iteration history
        2. Identify any remaining inconsistencies
        3. Suggest refinements for next iteration if needed

        Output:
        A single JSON object with three fields:
            • `match` (boolean):
                - `true` if all analyses align
                - `false` if there are genuine mismatches
            • `details` (string):
                - On success, empty string
                - On failure, list genuine missing or mismatched elements
            • `needs_refinement` (boolean):
                - `true` if another iteration might improve results
                - `false` if results are final or max iterations reached
        """

    logger.debug("\n" + "-"*50)
    logger.debug("Starting final verification loop")
    logger.debug("-"*50)

    # Get state and transition analysis results
    state_analysis = next((r for r in results if r['analysis_type'] == "State Analysis"), None)

    verification_history = []
    iteration = 0
    previous_details = ""

    while iteration < max_iterations:
        iteration += 1
        logger.debug(f"\nIteration {iteration}/{max_iterations}")

        # Build verification prompt with history
        history_context = "\n\nVerification History:\n" + "\n".join(
            f"Iteration {i+1}: {result['details']}"
            for i, result in enumerate(verification_history)
        ) if verification_history else ""

        verification_prompt = (
            system_prompt_reasoning +
            "\n\nState Analysis Results:\n" + state_analysis['details'] +
            history_context
        )

        verification_result = make_api_call(
            verification_prompt,
            f"Final Verification (Iteration {iteration})",
            attempt=1
        )

        if not verification_result:
            logger.error(f"Final verification failed on iteration {iteration}")
            verification_result = {
                "match": False,
                "details": f"Final verification could not be completed on iteration {iteration}",
                "needs_refinement": False
            }
            break

        verification_history.append(verification_result)

        # Check for convergence
        if verification_result['details'] == previous_details:
            logger.debug("Verification converged - no changes from previous iteration")
            break

        previous_details = verification_result['details']

        # Check if we need another iteration
        if not verification_result.get('needs_refinement', False):
            logger.debug("No further refinement needed")
            break

    # Use the last verification result
    verification_result = verification_history[-1] if verification_history else {
        "match": False,
        "details": "No verification results available",
        "iterations": 0
    }
    
    # Combine results with verification
    logger.debug("\n" + "-"*50)
    logger.debug("Analysis complete")
    logger.debug("-"*50 + "\n")

    # Format the results
    all_details = [
        f"{r['analysis_type']}:\n{r['details']}" for r in results
    ]

    # Add verification history
    if verification_history:
        all_details.append("\nVerification History:")
        for i, v in enumerate(verification_history, 1):
            all_details.append(f"\nIteration {i}:\n{v['details']}")

    # Overall match is true only if all analyses and final verification match
    final_match = (
        all(r.get('match', False) for r in results) and
        verification_result.get('match', False)
    )
    
    return {
        "match": final_match,
        "details": "\n\n".join(all_details),
        "iterations": len(verification_history)
    }



def main():
    parser = argparse.ArgumentParser(
        description="Compare C state machine vs PlantUML definition using OpenAI API",
        allow_abbrev=False,
    )
    parser.add_argument("--c-file", required=True, help="Path to the C source file")
    parser.add_argument("--uml-file", required=True, help="Path to the PlantUML file")
    parser.add_argument("--debug", action="store_true", help="Enable debug logging")
    args = parser.parse_args()

    c_code = load_file(args.c_file)
    plantuml = load_file(args.uml_file)

    result = compare_state_machines(c_code, plantuml, debug=args.debug)

    # Print results
    if result.get("match") is True:
        print("\n✅ State machines match")
    else:
        print("\n❌ State machines do not match")

    print("\nAnalysis Details:")
    print(result.get("details", "No details provided."))
    
    if "iterations" in result:
        print(f"\nVerification completed in {result['iterations']} iterations")
    
    sys.exit(0 if result.get("match") else 1)

if __name__ == "__main__":
    main()
