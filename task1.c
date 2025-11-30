#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MAX_USERS 5
#define MAX_RESOURCES 5
#define MAX_NAME_SIZE 20

typedef enum {
    READ = 1,
    WRITE = 2,
    EXECUTE = 4
} Permission;

typedef struct {
    char name[MAX_NAME_SIZE];
} User;

typedef struct {
    char name[MAX_NAME_SIZE];
} F_Resource;

typedef struct {
    char username[MAX_NAME_SIZE];
    int permissions; 
} ACLEntry;

typedef struct {
    F_Resource file_res;
    ACLEntry acl[MAX_USERS];
    int aclCount;
} ACLControlledResource;

typedef struct {
    char resourceName[MAX_NAME_SIZE];
    int permissions;
} Capability;

typedef struct {
    User user;
    Capability capabilities[MAX_RESOURCES];
    int capCount;
} CapabilityUser;

void printPermissions(int permit) {
    if (permit & READ) printf("READ");
    if (permit & WRITE) printf("WRITE");
    if (permit & EXECUTE) printf("EXECUTE");
}

int hasPermission(int userPermit, int requiredPermit) {
    return (userPermit & requiredPermit) == requiredPermit;
}

void checkACLAccess(ACLControlledResource *f_res, const char *userName, int permits) {
    for (int i = 0; i < f_res->aclCount; i++) {
        if (strcmp(f_res->acl[i].username, userName) == 0) {
            if (hasPermission(f_res->acl[i].permissions, permits)) {
                printf("ACL Check: User %s requests ", userName);
                printPermissions(permits);
                printf("on %s: Access GRANTED\n", f_res->file_res.name);
                return;
            } else {
                printf("ACL Check: User %s requests ", userName);
                printPermissions(permits);
                printf("on %s: Access DENIED\n", f_res->file_res.name);
                return;
            }
        }
    }
    printf("ACL Check: User %s has NO entry for resource %s: Access DENIED\n", userName, f_res->file_res.name);
}

void checkCapabilityAccess(CapabilityUser *user, const char *resourceName, int permits) {
    for (int i = 0; i < user->capCount; i++) {
        if (strcmp(user->capabilities[i].resourceName, resourceName) == 0) {
            if (hasPermission(user->capabilities[i].permissions, permits)) {
                printf("Capability Check: User %s requests ", user->user.name);
                printPermissions(permits);
                printf("on %s: Access GRANTED\n", resourceName);
                return;
            } else {
                printf("Capability Check: User %s requests ", user->user.name);
                printPermissions(permits);
                printf("on %s: Access DENIED\n", resourceName);
                return;
            }
        }
    }
    printf("Capability Check: User %s has NO capability for resource %s: Access DENIED\n", user->user.name, resourceName);
}

int main() {

    User users[MAX_USERS] = {{"Alice"}, {"Bob"}, {"Charlie"}, {"David"}, {"Eve"}};
    F_Resource resources[MAX_RESOURCES] = {{"File1"},{"File2"},{"File3"},{"File4"},{"File5"}};

    ACLControlledResource aclResources[MAX_RESOURCES] = {
        {resources[0], {{"Alice", READ|WRITE},{"Bob", READ},{"Charlie", 0}}, 3},
        {resources[1], {{"Alice", READ}, {"Charlie", READ | EXECUTE}}, 2},
        {resources[2], {{"Bob", WRITE}, {"Charlie", READ}}, 2},
        {resources[3], {{"David", READ | EXECUTE}, {"Eve", WRITE}}, 2},
        {resources[4], {{"Alice", READ}, {"Eve", READ | WRITE | EXECUTE}}, 2}
    };

    CapabilityUser capUsers[MAX_USERS] = {
        {users[0], {{"File1", READ|WRITE}, {"File2", READ}, {"File5",READ}}, 3},  
        {users[1], {{"File1", READ}, {"File3", WRITE}}, 2},                        
        {users[2], {{"File2", READ|EXECUTE}, {"File3",READ}}, 2},              
        {users[3], {{"File4", READ|EXECUTE}}, 1},                               
        {users[4], {{"File4", WRITE}, {"File5", READ|WRITE|EXECUTE}}, 2}      
    };

    checkACLAccess(&aclResources[0], "Alice", READ);
    checkACLAccess(&aclResources[0], "Bob", WRITE);
    checkACLAccess(&aclResources[0], "Charlie", READ);
    checkACLAccess(&aclResources[3], "David", EXECUTE);
    checkACLAccess(&aclResources[4], "Eve", WRITE);
    checkACLAccess(&aclResources[2], "Alice", EXECUTE);

    printf("\n");

    checkCapabilityAccess(&capUsers[0], "File1", WRITE);
    checkCapabilityAccess(&capUsers[1], "File1", WRITE);
    checkCapabilityAccess(&capUsers[2], "File2", EXECUTE);
    checkCapabilityAccess(&capUsers[3], "File4", READ);
    checkCapabilityAccess(&capUsers[4], "File5", EXECUTE);
    checkCapabilityAccess(&capUsers[0], "File3", READ);

    return 0;
}
