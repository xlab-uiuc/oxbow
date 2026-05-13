#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include "list.h" // Linux list.h header

// Define a structure for your linked list nodes
struct my_node {
	int data;
	struct list_head list;
};

// Initialize a head for your linked list
LIST_HEAD(my_list);

// Function to insert a new entry to the end of the linked list
void insert_at_end(int data)
{
	struct my_node *new_node =
		(struct my_node *)malloc(sizeof(struct my_node));
	if (new_node == NULL) {
		fprintf(stderr, "Memory allocation failed.\n");
		exit(EXIT_FAILURE);
	}

	new_node->data = data;
	INIT_LIST_HEAD(
		&new_node->list); // Initialize list head for the new node

	list_add_tail(&new_node->list, &my_list);
}

void get_last_entry()
{
	struct my_node *temp;
	temp = list_last_entry(&my_list, struct my_node, list);

	printf("Last entry: %d\n", temp->data);
}

// Function to print the linked list
void print_list()
{
	struct my_node *ptr;
	printf("Linked List: [");
	list_for_each_entry (ptr, &my_list, list) {
		printf("%d -> ", ptr->data);
	}
	printf("NULL] my_list.prev:%d\n",
	       list_entry(my_list.prev, struct my_node, list)->data);
}

int main()
{
	// Insert some elements at the end of the linked list
	print_list();
	insert_at_end(1);
	print_list();
	insert_at_end(2);
	print_list();
	insert_at_end(3);

	// Print the linked list
	print_list();

	get_last_entry();

	return 0;
}