#!/usr/bin/env python3

import os
import sys
from pathlib import Path
from openai import AzureOpenAI

def setup_client():
    """Initialize OpenAI client with Azure OpenAI """
    try:
        # Try Azure OpenAI
        return AzureOpenAI(
            api_version="2024-02-15-preview",
            api_key=os.getenv("OPENAI_API_KEY"),
            azure_endpoint=os.getenv("AZURE_OPENAI_ENDPOINT")
        )
    except Exception as e:
        print(f"Failed to initialize Azure OpenAI client: {e}")
        raise ValueError("No valid OpenAI credentials found. Please set Azure OpenAI environment variables")

# Initialize the client
client = setup_client()

def read_c_file(file_path):
    with open(file_path, 'r') as f:
        return f.read()

def generate_state_diagram(c_code):
    # Configure OpenAI API

    # System prompt to guide the analysis
    system_prompt = """
    You are a specialized state machine analyzer for Zephyr RTOS SMF framework C code.
    Your task is to generate precise PlantUML state diagrams by following these detailed rules:

    STATE IDENTIFICATION:
    1. Parse SMF_CREATE_STATE definitions with exact parameter analysis:
       - First param: Entry function (may contain state initialization logic)
       - Second param: Run function (contains state behavior and transitions)
       - Third/Fourth params: Usually NULL, but check for exit/parent functions
       - Fifth param: Critical for hierarchy - points to initial substate or NULL
    2. Extract state names from array indices in state definitions
    3. Identify initial states from smf_set_initial() calls

    TRANSITION ANALYSIS:
    1. Find all smf_set_state() calls within run functions
    2. Extract transition conditions by analyzing:
       - Complete if/switch statement conditions before smf_set_state()
       - Multiple conditions joined by && or ||
       - Channel checks (e.g., user_object->chan comparisons)
       - Status checks (e.g., user_object->status comparisons)
    4. Ignore transitions that return to the same state

    HIERARCHY MAPPING:
    1. Build parent-child relationships from:
       - Fifth parameter of SMF_CREATE_STATE (&states[CHILD])
       - Initial state settings within entry functions
    2. Properly nest states in PlantUML using:
       state PARENT {
           state CHILD {
               state GRANDCHILD
           }
       }
    3. Show initial transitions within each composite state: [*] --> FIRST_STATE

    PLANTUML OUTPUT:
    1. Output MUST be a raw PlantUML diagram without any markdown formatting
    2. Start with @startuml on first line and end with @enduml on last line
    3. Maintain proper state nesting and hierarchy
    4. Include initial state transitions at all levels
    5. Use proper PlantUML syntax for composite states
    6. Do not include any markdown code block markers (```) or other formatting

    CRITICAL RULES:
    1. Never invent or assume transitions - only use explicit STATE_SET calls
    3. Maintain precise parent-child relationships
    4. Include all state hierarchy levels
    5. Show all valid transitions between different states
    6. Ensure initial states are properly marked at each level

    Format output as PlantUML diagram only, no explanatory text.
    Follow the example structure below but adapt to the actual code patterns found.


    Example C code:
    ### C CODE INIT
/* State machine definition */
static const struct smf_state states[] = {
	[STATE_RUNNING] =
		SMF_CREATE_STATE(state_running_entry, state_running_run, NULL,
				 NULL,	/* No parent state */
				 &states[STATE_DISCONNECTED]),
	[STATE_DISCONNECTED] =
		SMF_CREATE_STATE(state_disconnected_entry, state_disconnected_run, NULL,
				 &states[STATE_RUNNING],
#if defined(CONFIG_APP_NETWORK_SEARCH_NETWORK_ON_STARTUP)
				 &states[STATE_DISCONNECTED_SEARCHING]),
#else
				 &states[STATE_DISCONNECTED_IDLE]),
#endif /* CONFIG_APP_NETWORK_SEARCH_NETWORK_ON_STARTUP */
	[STATE_DISCONNECTED_IDLE] =
		SMF_CREATE_STATE(NULL, state_disconnected_idle_run, NULL,
				 &states[STATE_DISCONNECTED],
				 NULL), /* No initial transition */
	[STATE_DISCONNECTED_SEARCHING] =
		SMF_CREATE_STATE(state_disconnected_searching_entry,
				 state_disconnected_searching_run, NULL,
				 &states[STATE_DISCONNECTED],
				 NULL), /* No initial transition */
	[STATE_CONNECTED] =
		SMF_CREATE_STATE(state_connected_entry, state_connected_run, NULL,
				 &states[STATE_RUNNING],
				 NULL), /* No initial transition */
	[STATE_DISCONNECTING] =
		SMF_CREATE_STATE(state_disconnecting_entry, state_disconnecting_run, NULL,
				 &states[STATE_RUNNING],
				 NULL), /* No initial transition */
};

static void state_running_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	LOG_DBG("state_running_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED]);
			break;
		case NETWORK_UICC_FAILURE:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);
			break;
		case NETWORK_SYSTEM_MODE_REQUEST:
			request_system_mode();
			break;
		default:
			break;
		}
	}
}


static void state_disconnected_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	LOG_DBG("state_disconnected_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {

		case NETWORK_CONNECTED:
			smf_set_state(SMF_CTX(state_object), &states[STATE_CONNECTED]);
			break;
		case NETWORK_DISCONNECTED:
			smf_set_handled(SMF_CTX(state_object));
			break;
		default:
			break;
		}
	}
}

static void state_disconnected_searching_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	LOG_DBG("state_disconnected_searching_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {

		case NETWORK_CONNECT:
			smf_set_handled(SMF_CTX(state_object));
			break;
		case NETWORK_SEARCH_STOP: __fallthrough;
		case NETWORK_DISCONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);
			break;
		default:
			break;
		}
	}
}

static void state_disconnected_idle_run(void *obj)
{
	int err;
	struct network_state_object const *state_object = obj;

	LOG_DBG("state_disconnected_idle_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECT:
			smf_set_handled(SMF_CTX(state_object));
			break;
		case NETWORK_CONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_SEARCHING]);
			break;
		case NETWORK_SYSTEM_MODE_SET_LTEM:
			err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_GPS,
						     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("lte_lc_system_mode_set, error: %d", err);
				SEND_FATAL_ERROR();
			}
			break;
		case NETWORK_SYSTEM_MODE_SET_NBIOT:
			err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_NBIOT_GPS,
						     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("lte_lc_system_mode_set, error: %d", err);
				SEND_FATAL_ERROR();
			}
			break;
		case NETWORK_SYSTEM_MODE_SET_LTEM_NBIOT:
			err = lte_lc_system_mode_set(LTE_LC_SYSTEM_MODE_LTEM_NBIOT_GPS,
						     LTE_LC_SYSTEM_MODE_PREFER_AUTO);
			if (err) {
				LOG_ERR("lte_lc_system_mode_set, error: %d", err);
				SEND_FATAL_ERROR();
			}
			break;
		default:
			break;
		}
	}
}

static void state_connected_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	LOG_DBG("state_connected_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		switch (msg.type) {
		case NETWORK_DISCONNECT:
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTING]);
			break;
		default:
			break;
		}
	}
}

static void state_disconnecting_run(void *obj)
{
	struct network_state_object const *state_object = obj;

	LOG_DBG("state_disconnecting_run");

	if (&NETWORK_CHAN == state_object->chan) {
		struct network_msg msg = MSG_TO_NETWORK_MSG(state_object->msg_buf);

		if (msg.type == NETWORK_DISCONNECTED) {
			smf_set_state(SMF_CTX(state_object), &states[STATE_DISCONNECTED_IDLE]);
		}
	}
}

	smf_set_initial(SMF_CTX(&network_state), &states[STATE_RUNNING]);


    ### C CODE END

    Example of desired PlantUML outcome in file:
@startuml
state STATE_RUNNING

[*] --> STATE_RUNNING

STATE_RUNNING --> STATE_DISCONNECTED: NETWORK_DISCONNECTED
STATE_RUNNING --> STATE_DISCONNECTED_IDLE: NETWORK_UICC_FAILURE

state STATE_RUNNING {
    state STATE_DISCONNECTED
    state STATE_CONNECTED
    state STATE_DISCONNECTING

    [*] --> STATE_DISCONNECTED

    STATE_DISCONNECTED --> STATE_CONNECTED: NETWORK_CONNECTED

    STATE_CONNECTED --> STATE_DISCONNECTING: NETWORK_DISCONNECT

    STATE_DISCONNECTING --> STATE_DISCONNECTED_IDLE: NETWORK_DISCONNECTED

    state STATE_DISCONNECTED {
        state STATE_DISCONNECTED_IDLE
        state STATE_DISCONNECTED_SEARCHING

        [*] --> STATE_DISCONNECTED_SEARCHING

        STATE_DISCONNECTED_SEARCHING --> STATE_DISCONNECTED_IDLE: NETWORK_SEARCH_STOP || NETWORK_DISCONNECT

        STATE_DISCONNECTED_IDLE --> STATE_DISCONNECTED_SEARCHING: NETWORK_CONNECT
    }
}
@enduml

    """

    # User prompt with the actual code
    user_prompt = f"Create a PlantUML state diagram from this C code:\n\n{c_code}"

    try:
        response = client.chat.completions.create(model="gpt-5.2",
        messages=[
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": user_prompt}
        ],
        # temperature=0.2,  # For consistent results
        # max_completion_tokens=4000,
        # seed=42,   # For consistent results
	)

        # Extract the PlantUML diagram
        plantuml_diagram = response.choices[0].message.content.strip()
        return plantuml_diagram

    except Exception as e:
        print(f"Error generating diagram: {e}")
        sys.exit(1)

def save_plantuml_diagram(diagram, output_file):
    with open(output_file, 'w') as f:
        f.write(diagram)

def main():
    if len(sys.argv) != 2:
        print("\nParses Zephyr RTOS SMF state machine C code and generates PlantUML diagram")
        print("\nOpenAI api backend")
        print("\nUsage: ./smf_to_plantuml.py <path_to_c_file>")
        print("Example: ./smf_to_plantuml.py src/modules/network/network_module.c")
        sys.exit(1)

    if 'OPENAI_API_KEY' not in os.environ or 'AZURE_OPENAI_ENDPOINT' not in os.environ:
        print("\nError: Required environment variables not set")
        print("Set with: export OPENAI_API_KEY='your-api-key'")
        print("         export AZURE_OPENAI_ENDPOINT='your-azure-endpoint'")
        sys.exit(1)

    # Get the input C file path
    c_file_path = Path(sys.argv[1])
    if not c_file_path.exists():
        print(f"Error: File {c_file_path} does not exist")
        sys.exit(1)

    # Read the C code
    c_code = read_c_file(c_file_path)

    print(c_code)

    # Generate the plantuml diagram
    plantuml_diagram = generate_state_diagram(c_code)

    print(plantuml_diagram)

    # Save the diagram using the C file's name
    base_name = os.path.splitext(os.path.basename(c_file_path))[0]
    output_file = base_name + '.puml'
    save_plantuml_diagram(plantuml_diagram, output_file)
    print(f"State machine diagram saved to {output_file}")

if __name__ == "__main__":
    main()
