#include "Q2_node.h"
#include <algorithm>
#include <iostream>
#include <tuple>

using namespace std;

static const int INF = 999;

static vector<tuple<string, string, string, int>> tableSnapshot(const routingtbl &tbl)
{
    vector<tuple<string, string, string, int>> snapshot;
    for (const RoutingEntry &entry : tbl.tbl)
    {
        snapshot.push_back(make_tuple(entry.dstip, entry.nexthop, entry.ip_interface, entry.cost));
    }
    sort(snapshot.begin(), snapshot.end());
    return snapshot;
}

static vector<vector<tuple<string, string, string, int>>> networkSnapshot(vector<RoutingNode *> nd)
{
    vector<vector<tuple<string, string, string, int>>> snapshot;
    for (RoutingNode *node : nd)
    {
        snapshot.push_back(tableSnapshot(node->getTable()));
    }
    return snapshot;
}

void printRT(vector<RoutingNode *> nd)
{
    /*Print routing table entries*/
    for (RoutingNode *node : nd)
    {
        node->printTable();
    }
}

void routingAlgo(vector<RoutingNode *> nd)
{
    bool changed = true;
    int rounds = 0;
    const int maxRounds = max(1, (int)nd.size() * (int)nd.size() * 4);

    while (changed && rounds < maxRounds)
    {
        vector<vector<tuple<string, string, string, int>>> before = networkSnapshot(nd);

        for (RoutingNode *node : nd)
        {
            node->sendMsg();
        }

        ++rounds;
        changed = (before != networkSnapshot(nd));
    }

    /*Print routing table entries after routing algo converges */
    printf("Printing the routing tables after the convergence \n");
    printRT(nd);
}

void RoutingNode::recvMsg(RouteMsg *msg)
{
    // Finding link cost
    int link_cost = -1;

    for (auto &i : interfaces)
    {
        if (get<0>(i).getip() == msg->recvip)
        {
            link_cost = get<2>(i);
            break;
        }
    }

    if (link_cost == -1)
    {
        cout << "ERROR: link cost not found for recvip = " << msg->recvip << endl;
        return;
    }

    routingtbl *recvRoutingTable = msg->mytbl;
    for (RoutingEntry entry : recvRoutingTable->tbl)
    {
        if (isMyInterface(entry.dstip))
            continue;

        int new_cost = (entry.cost >= INF - link_cost) ? INF : entry.cost + link_cost;
        // Check routing entry

        bool entryExists = false;
        for (size_t i = 0; i < mytbl.tbl.size(); ++i)
        {
            RoutingEntry myEntry = mytbl.tbl[i];
            // printf("i=%d, nodeRT.cost=%d, DV.cost=%d\n",i, myEntry.cost, entry.cost );
            if (myEntry.dstip == entry.dstip)
            {
                entryExists = true;
                // Poisoned reverse requires accepting worse updates from the
                // neighbor currently used as next hop.
                if (myEntry.nexthop == msg->from || myEntry.cost > new_cost)
                {
                    myEntry.cost = new_cost;
                    myEntry.nexthop = msg->from;
                    myEntry.ip_interface = msg->recvip;
                    mytbl.tbl[i] = myEntry;
                }
            }
        }
        if (!entryExists)
        {
            // add the new entry
            RoutingEntry newEntry;
            newEntry.dstip = entry.dstip;
            newEntry.nexthop = msg->from;
            newEntry.ip_interface = msg->recvip;
            newEntry.cost = new_cost;
            mytbl.tbl.push_back(newEntry);
        }
    }
}
