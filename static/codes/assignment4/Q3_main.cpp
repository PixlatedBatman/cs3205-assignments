#include "Q3_node.h"
#include <sstream>

vector<RoutingNode *> distanceVectorNodes;

void routingAlgo(vector<RoutingNode *> distanceVectorNodes);
// void routingAlgo2(vector<RoutingNode*> distanceVectorNodes);

int main()
{
    int n; // number of nodes
    cin >> n;
    string name; // Node label
    distanceVectorNodes.clear();
    for (int i = 0; i < n;)
    {
        cin >> name;
        if (i == 0 && n > 1 && name.length() == static_cast<size_t>(n))
        {
            for (int j = 0; j < n; ++j)
            {
                RoutingNode *newnode = new RoutingNode();
                newnode->setName(string(1, name[j]));
                distanceVectorNodes.push_back(newnode);
            }
            i = n;
            break;
        }

        RoutingNode *newnode = new RoutingNode();
        newnode->setName(name);
        distanceVectorNodes.push_back(newnode);
        ++i;
    }

    string line;
    getline(cin, line);

    /*
      Each topology line is:
      node own-interface-ip connected-interface-ip neighbor-node [link-cost]
      If link-cost is omitted, cost defaults to 1 so the original sample still works.
    */
    while (getline(cin, line))
    {
        if (line.empty())
            continue;

        istringstream iss(line);
        string myeth, oeth, oname;
        int cost = 1;
        iss >> name;

        if (name == "EOE")
            break;

        iss >> myeth >> oeth >> oname;
        if (!(iss >> cost))
            cost = 1;

        for (size_t i = 0; i < distanceVectorNodes.size(); i++)
        {
            if (distanceVectorNodes[i]->getName() != name)
                continue;

            for (size_t j = 0; j < distanceVectorNodes.size(); j++)
            {
                if (distanceVectorNodes[j]->getName() == oname)
                {
                    distanceVectorNodes[i]->addInterface(myeth, oeth, distanceVectorNodes[j], cost);
                    distanceVectorNodes[i]->addTblEntry(myeth, 0);
                    break;
                }
            }
            break;
        }
    }

    /* The logic of the routing algorithm should go here */
    routingAlgo(distanceVectorNodes);
    /* Add the logic for periodic update (after every 1 sec) here */
    // routingAlgo(distanceVectorNodes);
}
